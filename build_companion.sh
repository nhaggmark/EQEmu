#!/bin/bash
cd /home/eqemu/code/build && ninja -j4 2>&1 | tail -50
