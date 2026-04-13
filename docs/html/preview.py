#!/usr/bin/env python3
"""
VES 文档浏览器预览工具
在本地启动服务器并用浏览器打开文档
"""

import http.server
import socketserver
import webbrowser
import threading
import time
import os
import sys
import argparse

PORT = 8888  # 使用不同端口避免冲突
HTML_DIR = os.path.dirname(os.path.abspath(__file__))

class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HTML_DIR, **kwargs)
    
    def log_message(self, format, *args):
        # 静默日志
        pass

def check_port_available(port):
    """检查端口是否可用"""
    import socket
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("", port))
        return True
    except OSError:
        return False

def start_server(port):
    """启动HTTP服务器"""
    with socketserver.TCPServer(("", port), QuietHandler) as httpd:
        print(f"Server running at http://localhost:{port}/")
        httpd.serve_forever()

def main():
    parser = argparse.ArgumentParser(description='VES 文档浏览器预览')
    parser.add_argument('-p', '--port', type=int, default=PORT, help='服务器端口')
    parser.add_argument('-n', '--no-browser', action='store_true', help='不自动打开浏览器')
    args = parser.parse_args()
    
    port = args.port
    
    # 检查端口
    if not check_port_available(port):
        print(f"错误: 端口 {port} 已被占用")
        print("尝试寻找可用端口...")
        for p in range(port+1, port+100):
            if check_port_available(p):
                port = p
                print(f"使用端口: {port}")
                break
        else:
            print("无法找到可用端口")
            sys.exit(1)
    
    os.chdir(HTML_DIR)
    
    # 启动服务器
    server_thread = threading.Thread(target=start_server, args=(port,), daemon=True)
    server_thread.start()
    
    url = f"http://localhost:{port}/index.html"
    
    print("\n" + "="*50)
    print("VES 文档预览服务器")
    print("="*50)
    print(f"首页: {url}")
    print(f"目录: {HTML_DIR}")
    print("="*50)
    
    if not args.no_browser:
        time.sleep(0.5)
        print("\n正在打开浏览器...")
        webbrowser.open(url)
    
    print("\n按 Ctrl+C 停止服务器\n")
    
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n服务器已停止")

if __name__ == "__main__":
    main()
