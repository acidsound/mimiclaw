#!/usr/bin/env bash

SESSION="esp"
PORT="/dev/cu.usbmodem101"
BAUD="115200"

# 프로젝트 루트 기준
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IDF_PATH="$HOME/esp/esp-idf"

cd "$PROJECT_DIR" || exit 1

# tmux 세션 없으면 생성
if ! tmux has-session -t $SESSION 2>/dev/null; then
  tmux new-session -d -s $SESSION -c "$PROJECT_DIR"

  # Pane 0: Monitor
  tmux send-keys -t $SESSION:0.0 "
export IDF_TOOLS_PATH=\"$PROJECT_DIR/.espressif\"
. $IDF_PATH/export.sh
idf.py -p $PORT monitor
" C-m

  # Pane 1: Build/Flash 용
  tmux split-window -h -t $SESSION:0 -c "$PROJECT_DIR"

  tmux send-keys -t $SESSION:0.1 "
export IDF_TOOLS_PATH=\"$PROJECT_DIR/.espressif\"
. $IDF_PATH/export.sh
echo 'Build/Flash pane ready'
" C-m

  tmux select-pane -t $SESSION:0.1
fi

tmux attach -t $SESSION
