#! /bin/bash

set -e

TEST_DIR="$(dirname "$(realpath "$0")")"
REPO_DIR="$TEST_DIR/../.."
BUILD_DIR="$REPO_DIR/build"
OBSERVER="$BUILD_DIR/bin/observer"
OBCLIENT="$BUILD_DIR/bin/obclient"
TABLE_DATA="$1"

if [ -z "$TABLE_DATA" ]; then
    TABLE_DATA="miniob-sample.table"
fi

info() {
    echo -e "\e[1m> $@\e[0m"
}

run_observer() {
    info "start observer..."
    cd $BUILD_DIR
    rm -rf $BUILD_DIR/miniob
    $OBSERVER &
    OBSERVER_PID=$!
    sleep 2s
    info "run observer on $OBSERVER_PID"
}

sql() {
    info "$@"
    echo "$@" | $OBCLIENT || true
    echo
}

on_exit() {
    if [ -n "$OBSERVER_PID" ]; then
        info "kill observer"
        kill -9 $OBSERVER_PID
    fi
}
trap on_exit EXIT


if [ -z "$(pgrep observer)" ]; then
    run_observer
fi

# paylaod
sql "create table t1 (c1 int, c2 int, c3 int, v1 char, v2 char, v3 char, v4 char, v5 char, v6 char, v7 char, v8 char, v9 char);"
sql "load data infile '$TEST_DIR/$TABLE_DATA' into table t1;"
sql "create index i1 on t1(c2);"
sql "select * from t1 where c1 = 1;"