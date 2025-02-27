#!/usr/bin/env bash

# Copyright 2021 4Paradigm
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

sh openmldb-ns-1/bin/start.sh stop nameserver
sh openmldb-ns-2/bin/start.sh stop nameserver
sh openmldb-tablet-1/bin/start.sh stop tablet
sh openmldb-tablet-2/bin/start.sh stop tablet
sh openmldb-tablet-3/bin/start.sh stop tablet
sh openmldb-apiserver-1/bin/start.sh stop apiserver
sh zookeeper-3.4.14/bin/zkServer.sh stop

cp -r openmldb fedb-ns-1/bin/
cp -r openmldb fedb-ns-2/bin/
cp -r openmldb fedb-tablet-1/bin/
cp -r openmldb fedb-tablet-2/bin/
cp -r openmldb fedb-tablet-3/bin/
cp -r openmldb fedb-apiserver-1/bin/
