#!/bin/bash

# 快速测试脚本

PORT=${1:-9000}

echo "=== 测试WebSocket服务器 (端口: $PORT) ==="
echo "确保服务器正在运行..."
echo ""

# 检查服务器是否在运行
if ! netstat -ln | grep -q ":$PORT "; then
    echo "警告: 服务器可能没有在端口 $PORT 上运行"
    echo "请先启动服务器:"
    echo "  ./run_server.sh $PORT"
    echo "或者:"
    echo "  ./target/release/websocket_server $PORT"
    echo ""
fi

echo "运行Python测试..."
python3 simple_test_server.py $PORT
