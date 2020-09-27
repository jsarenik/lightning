#!/bin/sh
#
# This is like travis_wait but simpler

echo $1 | grep -q '[0-9]\+' || {
	echo "The first parameter has to be the maximum time in seconds." >&2
	exit 1
}

MAXTIME=${1}; shift
timeout $MAXTIME $* </dev/null &
PID=$!
trap "kill $PID; wait $PID; RET=$?; kill $$; exit $RET" QUIT INT
while ps -q $PID 2>&1 >/dev/null && printf ' \b'
do sleep 60; done
wait $PID
