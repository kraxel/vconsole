#!/bin/sh
sed	-e 's/"/\\"/g' \
	-e 's/\(.*\)/"\1\\n"/'	\
	< "$1" > "$2"
