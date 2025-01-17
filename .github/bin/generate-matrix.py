#!/usr/bin/env python3

# Copyright 2021 The Verible Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path

matrix = []

with (Path(__file__).parent.resolve().parent.parent / 'releasing' / 'supported_bases.txt').open('r') as fptr:
    for items in [line.strip().split(':') for line in fptr.readlines()]:
        for arch in ["x86_64", "arm64"]:
            matrix.append({
                'os': items[0],
                'ver': items[1],
                'arch': arch
            })

print('::set-output name=matrix::' + str(matrix))
