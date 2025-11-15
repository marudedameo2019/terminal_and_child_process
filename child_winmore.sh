#!/bin/sh
for i in $(seq 100); do
    echo $i
done | sed 's/$/\r/' | more.com
