#!/bin/sh

#
# Copyright 2020-2021 Greg Padiasek and Seegnify <http://www.seegnify.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#


start() {
  PORT=$1
  IMPL=$2
  CMD=./build/seegnify-training
  LIB=./build/lib$IMPL.so
  echo Starting master $IMPL at port $PORT
  nice -n 10 $CMD master $IMPL.graph $PORT >> $IMPL.master.log 2>&1 &
  sleep 1
  echo Starting worker $IMPL at port $PORT
  nice -n 10 $CMD worker 127.0.0.1 $PORT $LIB >> $IMPL.client.log 2>&1 &
  #valgrind $CMD worker 127.0.0.1 $PORT $LIB >> $IMPL.client.log 2>&1 &
}

ulimit -c unlimited

#start 9090 example-transformer
start 9191 example-regression
