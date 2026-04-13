#!/usr/bin/env python3
"""
VES 文档本地测试服务器
自动启动HTTP服务器并打开浏览器验证HTML文档
"""

import http.server
import socketserver
import webbrowser
import threading
import time
import os
import sys

PORT = 8000
HTML_DIR = os.path.dirname(os.path.abspath(__file__))

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HTML_DIR, **kwargs)
    
    def log_message(self, format, *args):
        # 简化日志输出
        print(f"[{self.log_date_time_string()}] {args[0]}")

def start_server():
    """启动HTTP服务器"""
    with socketserver.TCPServer(("", PORT), Handler) as httpd:
        print(f"\n{'='*50}")
        print(f"HTTP服务器已启动: http://localhost:{PORT}")
        print(f"文档根目录: {HTML_DIR}")
        print(f"{'='*50}\n")
        httpd.serve_forever()

def open_browser():
    """延迟后打开浏览器"""
    time.sleep(1.5)  # 等待服务器启动
    
    pages = [
        ("首页", "http://localhost:8000/index.html"),
        ("整体架构", "http://localhost:8000/architecture.html"),
        ("scanFile调用链", "http://localhost:8000/scanfile-flow.html"),
        ("异常处理", "http://localhost:8000/exception-handling.html"),
        ("冷却机制", "http://localhost:8000/cooldown.html"),
        ("自启与恢复", "http://localhost:8000/recovery.html"),
    ]
    
    print("\n正在打开浏览器验证以下页面：")
    for name, url in pages:
        print(f"  - {name}: {url}")
        webbrowser.open_new_tab(url)
        time.sleep(0.5)
    
    print(f"\n{'='*50}")
    print("所有页面已打开，请检查浏览器中的显示效果")
    print(f"按 Ctrl+C 停止服务器")
    print(f"{'='*50}\n")

def check_files():
    """检查关键文件是否存在"""
    print("\n检查文档文件...")
    files = [
        "index.html", "architecture.html", "scanfile-flow.html",
        "exception-handling.html", "cooldown.html", "recovery.html",
        "css/style.css"
    ]
    
    all_ok = True
    for f in files:
        path = os.path.join(HTML_DIR, f)
        exists = os.path.exists(path)
        status = "✓" if exists else "✗"
        print(f"  {status} {f}")
        if not exists:
            all_ok = False
    
    return all_ok

if __name__ == "__main__":
    os.chdir(HTML_DIR)
    
    print("="*50)
    print("VES 文档本地测试工具")
    print("="*50)
    
    if not check_files():
        print("\n错误：部分文件缺失！")
        sys.exit(1)
    
    # 启动服务器线程
    server_thread = threading.Thread(target=start_server, daemon=True)
    server_thread.start()
    
    # 启动浏览器
    open_browser()
    
    # 保持主线程运行
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\n服务器已停止")
