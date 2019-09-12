docker build -f jenkins/Dockerfile.dev -t yosys .
docker run -v $(pwd):/yosys yosys bash -c "./yosys/jenkins/install.sh"