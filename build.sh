#!/bin/bash
port=$1
example=$2
repos=`echo $example | sed -E -n 's|(.*)/.*$|\1|p'`

create_dir() {
  if [ -d $1 ]; then
    mkdir -p $1
  fi;
}

create_repo() {
  create_dir $1
  pwd=`pwd`
  cd $1
  git init --bare
  git branch -m main
  git config --file config http.receivepack true
  git symbolic-ref HEAD refs/heads/main
  cd $pwd
}

build_git_server() {
  gcc git_http_server.c -o bin/git_http_server
}

create_service_file() {
  pwd=`pwd`
  port=$1
  repos=$2

echo """
[Unit]
Description=Git-Server-Service

[Service]
Restart=always
RestartSec=3
User=user
ExecStart=bash -c \"cd $pwd; ./bin/git_http_server $port $repos\"
ExecReload=echo Reloaded

[Install]
WantedBy=default.target
WantedBy=multi-user.target
""" > ./git_server.service

}

run_git_server() {
  ./bin/git_http_server $1 $2
}
