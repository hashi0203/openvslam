#!/bin/bash

if [ $# -lt 1 ]; then
  echo "指定された引数は$#個です。" 1>&2
  echo "実行するには1個の引数が必要です。" 1>&2
  exit 1
fi

file=video
opt=0
if [ $# -gt 1 ]; then
  expr $2 + 1 > /dev/null 2>&1
  RET1=$?
  echo "戻り値 : $2"
  echo "戻り値 : $RET1"
  if [ $RET1 -lt 2 ]; then
    opt=${2}
  else
    file=${2}
  fi
fi

if [ $# -gt 2 ]; then
  expr ${3} + 1 > /dev/null 2>&1
  RET2=$?
  if [ $RET2 -lt 2 ] && [ $RET1 -ge 2 ]; then
    opt=${3}
  elif [ $RET2 -ge 2 ] && [ $RET1 -lt 2 ]; then
    file=${3}
  else
    echo "invalid argument" 1>&2
    exit 1
  fi
fi

./run_video_slam -v orb_vocab/orb_vocab.dbow2 -c videos/${1}/config.yaml -m videos/${1}/${file}.mov --frame-skip 3 --map-db videos/${1}_map.msg --proc-opt ${opt}
