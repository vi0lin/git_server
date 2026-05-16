# git_server

# Installation
```bash
git clone https://github.com/vi0lin/git_server.git
(
  cd git_server && source build.sh
  port=3000
  example=~/git_repos/my_repo.git
  repos=~/git_repos/
  create_dir $repos
  create_repo $example
  create_service_file $port $repos
  build_git_server
  run_git_server $port $repos
)
```

# Usage
```bash
git clone http://localhost:3000/my_repo.git
```
