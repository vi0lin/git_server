# git_server

# Installation
```bash
git clone https://github.com/vi0lin/git_server.git
cd git_server
```

# build.sh usage
```bash
source build.sh
port=3000
example=~/git_repos/my_repo.git
repos=~/git_repos
create_dir $repos
create_repo $example
create_service_file $port $repos
install_service
build_git_server
run_git_server $port $repos
```

# Build
```bash
gcc git_http_server.c -o bin/git_http_server
```

# Run 
```bash
./bin/git_http_server 3000 ~/git_repos
```

# Usage
```bash
git clone http://localhost:3000/my_repo.git
```

# Adding Repositories
```bash
cd ~/git_repos
mkdir example.git && cd example.git
git init --bare
git config --file config http.receivepack true
git symbolic-ref HEAD refs/heads/main
```

# Setup Service
```bash
source build.sh
create_service_file 3000 ~/git_repos
sudo chmod u+x ./git_server.service
ln -s -t /etc/systemd/system git_server.service
systemctl daemon-reload
systemctl enable git_server
systemctl start git_server
```
