#!/bin/bash

# 构建并测试WebSocket服务器脚本

echo "=== 构建WebSocket服务器 ==="
cargo build --release --features websocket --bin websocket_server

if [ $? -ne 0 ]; then
    echo "构建失败!"
    exit 1
fi

echo ""
echo "=== 服务器构建成功 ==="
echo "可执行文件位置: ./target/release/websocket_server"
echo "默认端口: 9000"
echo ""

# 检查是否提供了端口参数
PORT=${1:-9000}

echo "=== 启动服务器 (端口: $PORT) ==="
echo "按 Ctrl+C 停止服务器"
echo ""

# 启动服务器
./target/release/websocket_server $PORT
