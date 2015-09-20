/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

#undef LOG_MATRICES
#undef PYPLOT_EDGES

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static uint32_t xorshift32_state;

static double xorshift32()
{
	xorshift32_state ^= xorshift32_state << 13;
	xorshift32_state ^= xorshift32_state >> 17;
	xorshift32_state ^= xorshift32_state << 5;
	return (xorshift32_state % 1000000) / 1e6;
}

struct QwpConfig
{
	bool ltr;
	bool alpha;
	double grid;

	QwpConfig() {
		ltr = false;
		alpha = false;
		grid = 1.0 / 16;
	}
};

struct QwpWorker
{
	const QwpConfig &config;
	Module *module;
	char direction;

	struct Node {
		Cell *cell;
		bool tied, alt_tied;

		// pos = position in current direction
		// alt_pos = position in the other direction
		double pos, alt_pos;

		Node() {
			cell = nullptr;
			tied = false;
			pos = xorshift32();
			alt_tied = false;
			alt_pos = xorshift32();
		}

		void tie(double v) {
			tied = true;
			pos = v;
		}

		void alt_tie(double v) {
			alt_tied = true;
			alt_pos = v;
		}

		void swap_alt() {
			std::swap(tied, alt_tied);
			std::swap(pos, alt_pos);
		}

		void proj_left(double midpos) {
			cell = nullptr;
			tie(pos > midpos ? midpos : pos);
		}

		void proj_right(double midpos) {
			cell = nullptr;
			tie(pos < midpos ? midpos : pos);
		}
	};

	vector<Node> nodes;
	dict<pair<int, int>, double> edges;
	dict<Cell*, int> cell_to_node;

	QwpWorker(const QwpConfig &config, Module *module, char direction = 'x') : config(config), module(module), direction(direction)
	{
		log_assert(direction == 'x' || direction == 'y');
	}

	void load_module()
	{
		log_assert(direction == 'x');

		SigMap sigmap(module);
		dict<SigBit, pool<int>> bits_to_nodes;

		if (config.ltr || config.alpha)
		{
			dict<Wire*, double> alpha_inputs, alpha_outputs;

			if (config.alpha)
			{
				dict<string, Wire*> alpha_order;

				for (auto wire : module->wires()) {
					if (wire->port_input || wire->port_output)
						alpha_order[wire->name.str()] = wire;
				}

				alpha_order.sort();

				for (auto &it : alpha_order) {
					if (it.second->port_input) {
						int idx = GetSize(alpha_inputs);
						alpha_inputs[it.second] = idx + 0.5;
					}
					if (it.second->port_output) {
						int idx = GetSize(alpha_outputs);
						alpha_outputs[it.second] = idx + 0.5;
					}
				}
			}

			for (auto wire : module->wires())
			{
				if (!wire->port_input && !wire->port_output)
					continue;

				int idx = GetSize(nodes);
				nodes.push_back(Node());

				if (config.ltr) {
					if (wire->port_input)
						nodes[idx].tie(0.0);
					else
						nodes[idx].tie(1.0);
				}

				if (config.alpha) {
					if (wire->port_input)
						nodes[idx].alt_tie(alpha_inputs.at(wire) / GetSize(alpha_inputs));
					else
						nodes[idx].alt_tie(alpha_outputs.at(wire) / GetSize(alpha_outputs));
				}

				for (auto bit : sigmap(wire))
					bits_to_nodes[bit].insert(idx);
			}
		}

		for (auto cell : module->selected_cells())
		{
			log_assert(cell_to_node.count(cell) == 0);
			int idx = GetSize(nodes);
			nodes.push_back(Node());

			cell_to_node[cell] = GetSize(nodes);
			nodes[idx].cell = cell;

			for (auto &conn : cell->connections())
			for (auto bit : sigmap(conn.second))
				bits_to_nodes[bit].insert(idx);
		}

		for (auto &it : bits_to_nodes)
		{
			if (GetSize(it.second) > 100)
				continue;

			for (int idx1 : it.second)
			for (int idx2 : it.second)
				if (idx1 < idx2)
					edges[pair<int, int>(idx1, idx2)] += 1.0 / GetSize(it.second);
		}
	}

	void solve()
	{
		int observation_matrix_m = GetSize(edges) + GetSize(nodes);
		int observation_matrix_n = GetSize(nodes);

		// Column-major order
		vector<double> observation_matrix(observation_matrix_m * observation_matrix_n);
		vector<double> observation_rhs_vector(observation_matrix_m);

		int i = 0;
		for (auto &edge : edges) {
			int idx1 = edge.first.first;
			int idx2 = edge.first.second;
			double weight = edge.second * (1.0 + xorshift32() * 1e-3);
			observation_matrix[i + observation_matrix_m*idx1] = weight;
			observation_matrix[i + observation_matrix_m*idx2] = -weight;
			i++;
		}

		int j = 0;
		for (auto &node : nodes) {
			double weight = 1e-6;
			if (node.tied) weight = 1e3;
			weight *= (1.0 + xorshift32() * 1e-3);
			observation_matrix[i + observation_matrix_m*j] = weight;
			observation_rhs_vector[i] = node.pos * weight;
			i++, j++;
		}

#ifdef LOG_MATRICES
		log("----\n");
		for (int i = 0; i < observation_matrix_m; i++) {
			for (int j = 0; j < observation_matrix_n; j++)
				log(" %10.2e", observation_matrix[i + observation_matrix_m*j]);
			log(" |%9.2e\n", observation_rhs_vector[i]);
		}
#endif

		// A := observation_matrix
		// y := observation_rhs_vector
		//
		// AA = A' * A
		// Ay = A' * y
		//
		// M := [AA Ay]

		// Row major order
		vector<double> M(observation_matrix_n * (observation_matrix_n+1));
		int N = observation_matrix_n;

		for (int i = 0; i < N; i++)
		for (int j = 0; j < N; j++) {
			double sum = 0;
			for (int k = 0; k < observation_matrix_m; k++)
				sum += observation_matrix[k + observation_matrix_m*i] * observation_matrix[k + observation_matrix_m*j];
			M[(N+1)*i + j] = sum;
		}

		for (int i = 0; i < N; i++) {
			double sum = 0;
			for (int k = 0; k < observation_matrix_m; k++)
				sum += observation_matrix[k + observation_matrix_m*i] * observation_rhs_vector[k];
			M[(N+1)*i + N] = sum;
		}

#ifdef LOG_MATRICES
		log("\n");
		for (int i = 0; i < N; i++) {
			for (int j = 0; j < N+1; j++)
				log(" %10.2e", M[(N+1)*i + j]);
			log("\n");
		}
#endif

		// Solve "AA*x = Ay"
		// (least squares fit for "A*x = y")
		//
		// Using gaussian elimination (no pivoting) to get M := [Id x]

		// eliminate to upper triangular matrix
		for (int i = 0; i < N; i++)
		{
			// normalize row
			for (int j = i+1; j < N+1; j++)
				M[(N+1)*i + j] /= M[(N+1)*i + i];
			M[(N+1)*i + i] = 1.0;

			// elimination
			for (int j = i+1; j < N; j++) {
				double d = M[(N+1)*j + i];
				for (int k = 0; k < N+1; k++)
					if (k > i)
						M[(N+1)*j + k] -= d*M[(N+1)*i + k];
					else
						M[(N+1)*j + k] = 0.0;
			}
		}

		// back substitution
		for (int i = N-1; i >= 0; i--)
		for (int j = i+1; j < N; j++)
		{
			M[(N+1)*i + N] -= M[(N+1)*i + j] * M[(N+1)*j + N];
			M[(N+1)*i + j] = 0.0;
		}

#ifdef LOG_MATRICES
		log("\n");
		for (int i = 0; i < N; i++) {
			for (int j = 0; j < N+1; j++)
				log(" %10.2e", M[(N+1)*i + j]);
			log("\n");
		}
#endif

		// update node positions
		for (int i = 0; i < N; i++)
			if (!nodes[i].tied)
				nodes[i].pos = M[(N+1)*i + N];
	}

	void log_cell_coordinates(int indent, bool log_all_nodes = false)
	{
		for (auto &node : nodes)
		{
			if (node.cell == nullptr && !log_all_nodes)
				continue;

			for (int i = 0; i < indent; i++)
				log("  ");

			if (direction == 'x')
				log("X=%.2f, Y=%.2f", node.pos, node.alt_pos);
			else
				log("X=%.2f, Y=%.2f", node.alt_pos, node.pos);

			if (node.tied)
				log(" [%c-tied]", direction);

			if (node.alt_tied)
				log(" [%c-tied]", direction == 'x' ? 'y' : 'x');

			if (node.cell != nullptr)
				log(" %s (%s)", log_id(node.cell), log_id(node.cell->type));
			else
				log(" (none)");

			log("\n");
		}
	}

	void run_worker(int indent, double midpos, double radius, double alt_midpos, double alt_radius)
	{
		int count_cells = 0;

		for (auto &node : nodes)
			if (node.cell != nullptr)
				count_cells++;

		for (int i = 0; i < indent; i++)
			log("  ");

		if (direction == 'x')
			log("x-qwp on X=%.2f:%.2f, Y=%.2f:%.2f with %d cells, %d nodes, and %d edges.\n",
					midpos - radius, midpos + radius, alt_midpos - alt_radius, alt_midpos + alt_radius,
					count_cells, GetSize(nodes), GetSize(edges));
		else
			log("y-qwp on X=%.2f:%.2f, Y=%.2f:%.2f with %d cells, %d nodes, and %d edges.\n",
					alt_midpos - alt_radius, alt_midpos + alt_radius, midpos - radius, midpos + radius,
					count_cells, GetSize(nodes), GetSize(edges));

		solve();

		for (auto &node : nodes) {
			log_assert(node.pos + 0.1 >= midpos - radius);
			log_assert(node.pos - 0.1 <= midpos + radius);
			log_assert(node.alt_pos + 0.1 >= alt_midpos - alt_radius);
			log_assert(node.alt_pos - 0.1 <= alt_midpos + alt_radius);
		}

		if (2*radius <= config.grid && 2*alt_radius <= config.grid) {
			log_cell_coordinates(indent + 1);
			return;
		}

		// detect median position

		vector<pair<double, int>> sorted_pos;
		for (int i = 0; i < GetSize(nodes); i++)
			if (nodes[i].cell != nullptr)
				sorted_pos.push_back(pair<double, int>(nodes[i].pos, i));

		if (GetSize(sorted_pos) < 2) {
			log_cell_coordinates(indent + 1);
			return;
		}

		std::sort(sorted_pos.begin(), sorted_pos.end());

		// create child workers

		char child_direction = direction == 'x' ? 'y' : 'x';

		QwpWorker left_worker(config, module, child_direction);
		QwpWorker right_worker(config, module, child_direction);

		// duplicate nodes into child workers

		dict<int, int> left_nodes, right_nodes;

		for (int k = 0; k < GetSize(sorted_pos); k++)
		{
			int i = sorted_pos[k].second;

			if (k < GetSize(sorted_pos) / 2) {
				left_nodes[i] = GetSize(left_worker.nodes);
				left_worker.nodes.push_back(nodes[i]);
				if (left_worker.nodes.back().pos > midpos)
					left_worker.nodes.back().pos = midpos;
				left_worker.nodes.back().swap_alt();
			} else {
				right_nodes[i] = GetSize(right_worker.nodes);
				right_worker.nodes.push_back(nodes[i]);
				if (right_worker.nodes.back().pos < midpos)
					right_worker.nodes.back().pos = midpos;
				right_worker.nodes.back().swap_alt();
			}
		}

		// duplicate edges into child workers, project nodes as needed

		for (auto &edge : edges)
		{
			int idx1 = edge.first.first;
			int idx2 = edge.first.second;
			double weight = edge.second;

			if (nodes[idx1].cell == nullptr && nodes[idx2].cell == nullptr)
				continue;

			int left_idx1 = left_nodes.count(idx1) ? left_nodes.at(idx1) : -1;
			int left_idx2 = left_nodes.count(idx2) ? left_nodes.at(idx2) : -1;

			int right_idx1 = right_nodes.count(idx1) ? right_nodes.at(idx1) : -1;
			int right_idx2 = right_nodes.count(idx2) ? right_nodes.at(idx2) : -1;

			if (nodes[idx1].cell && left_idx1 >= 0 && left_idx2 < 0) {
				left_idx2 = left_nodes[idx2] = GetSize(left_worker.nodes);
				left_worker.nodes.push_back(nodes[idx2]);
				left_worker.nodes.back().proj_left(midpos);
				left_worker.nodes.back().swap_alt();
			} else
			if (nodes[idx2].cell && left_idx2 >= 0 && left_idx1 < 0) {
				left_idx1 = left_nodes[idx1] = GetSize(left_worker.nodes);
				left_worker.nodes.push_back(nodes[idx1]);
				left_worker.nodes.back().proj_left(midpos);
				left_worker.nodes.back().swap_alt();
			}

			if (nodes[idx1].cell && right_idx1 >= 0 && right_idx2 < 0) {
				right_idx2 = right_nodes[idx2] = GetSize(right_worker.nodes);
				right_worker.nodes.push_back(nodes[idx2]);
				right_worker.nodes.back().proj_right(midpos);
				right_worker.nodes.back().swap_alt();
			} else
			if (nodes[idx2].cell && right_idx2 >= 0 && right_idx1 < 0) {
				right_idx1 = right_nodes[idx1] = GetSize(right_worker.nodes);
				right_worker.nodes.push_back(nodes[idx1]);
				right_worker.nodes.back().proj_right(midpos);
				right_worker.nodes.back().swap_alt();
			}

			if (left_idx1 >= 0 && left_idx2 >= 0)
				left_worker.edges[pair<int, int>(left_idx1, left_idx2)] += weight;

			if (right_idx1 >= 0 && right_idx2 >= 0)
				right_worker.edges[pair<int, int>(right_idx1, right_idx2)] += weight;
		}

		// run child workers

		left_worker.run_worker(indent+1, alt_midpos, alt_radius, midpos - radius/2, radius/2);
		right_worker.run_worker(indent+1, alt_midpos, alt_radius, midpos + radius/2, radius/2);

		// re-integrate results

		for (auto &it : left_nodes)
			if (left_worker.nodes[it.second].cell != nullptr) {
				nodes[it.first].pos = left_worker.nodes[it.second].alt_pos;
				nodes[it.first].alt_pos = left_worker.nodes[it.second].pos;
			}

		for (auto &it : right_nodes)
			if (right_worker.nodes[it.second].cell != nullptr) {
				nodes[it.first].pos = right_worker.nodes[it.second].alt_pos;
				nodes[it.first].alt_pos = right_worker.nodes[it.second].pos;
			}
	}

	void run()
	{
		log("Running qwp on module %s..\n", log_id(module));

		load_module();
		run_worker(1, 0.5, 0.5, 0.5, 0.5);

		for (auto &node : nodes)
			if (node.cell != nullptr)
				node.cell->attributes["\\qwp_position"] = stringf("%f %f", node.pos, node.alt_pos);
	}
};

struct QwpPass : public Pass {
	QwpPass() : Pass("qwp", "quadratic wirelength placer") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    qwp [options] [selection]\n");
		log("\n");
		log("This command runs quadratic wirelength placement on the selected modules and\n");
		log("annotates the cells in the design with 'qwp_position' attributes.\n");
		log("\n");
		log("    -ltr\n");
		log("        Add left-to-right constraints: constrain all inputs on the left border\n");
		log("        outputs to the right border.\n");
		log("\n");
		log("    -alpha\n");
		log("        Add constraints for inputs/outputs to be placed in alphanumerical\n");
		log("        order along the y-axis (top-to-bottom).\n");
		log("\n");
		log("    -grid N\n");
		log("        Number of grid divisions in x- and y-direction. (default=16)\n");
		log("\n");
		log("Note: This implementation of a quadratic wirelength placer uses unoptimized\n");
		log("dense matrix operations. It is only a toy-placer for small circuits.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		QwpConfig config;
		xorshift32_state = 123456789;

		log_header("Executing QWP pass (quadratic wirelength placer).\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-ltr") {
				config.ltr = true;
				continue;
			}
			if (args[argidx] == "-alpha") {
				config.alpha = true;
				continue;
			}
			if (args[argidx] == "-grid" && argidx+1 < args.size()) {
				config.grid = 1.0 / atoi(args[++argidx].c_str());
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		for (auto module : design->selected_modules())
		{
			QwpWorker worker(config, module);
			worker.run();

#ifdef PYPLOT_EDGES
			log("\n");
			log("plt.figure(figsize=(10, 10));\n");

			for (auto &edge : worker.edges) {
				log("plt.plot([%.2f, %.2f], [%.2f, %.2f], \"r-\");\n",
						worker.nodes[edge.first.first].pos,
						worker.nodes[edge.first.second].pos,
						worker.nodes[edge.first.first].alt_pos,
						worker.nodes[edge.first.second].alt_pos);
			}

			for (auto &node : worker.nodes) {
				const char *style = node.cell != nullptr ? "ko" : "ks";
				log("plt.plot([%.2f], [%.2f], \"%s\");\n", node.pos, node.alt_pos, style);
			}
#endif
		}
	}
} QwpPass;

PRIVATE_NAMESPACE_END