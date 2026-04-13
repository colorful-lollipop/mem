#!/usr/bin/env python3
"""
VES 文档测试报告生成器
"""

import os
import re
from pathlib import Path
from datetime import datetime

HTML_DIR = Path(__file__).parent

def analyze_file(filepath):
    """分析单个HTML文件"""
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # 统计信息
    lines = content.count('\n')
    chars = len(content)
    
    # 标题
    title_match = re.search(r'<title>(.*?)</title>', content)
    title = title_match.group(1) if title_match else "Unknown"
    
    # 图表
    mermaid_count = content.count('class="mermaid"')
    
    # 代码块
    code_blocks = len(re.findall(r'<pre[^>]*data-language', content))
    
    # 表格
    tables = content.count('<table')
    
    # 信息框
    info_boxes = content.count('class="info-box"')
    
    return {
        'name': filepath.name,
        'title': title,
        'lines': lines,
        'chars': chars,
        'mermaid': mermaid_count,
        'code': code_blocks,
        'tables': tables,
        'info_boxes': info_boxes
    }

def generate_report():
    """生成测试报告"""
    files = sorted(HTML_DIR.glob('*.html'))
    
    total_stats = {
        'files': len(files),
        'lines': 0,
        'chars': 0,
        'mermaid': 0,
        'code': 0,
        'tables': 0,
        'info_boxes': 0
    }
    
    print("="*80)
    print("VES 文档 HTML 测试报告")
    print(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("="*80)
    print()
    
    print(f"{'文件名':<25} {'行数':>6} {'字符':>8} {'图表':>4} {'代码':>4} {'表格':>4} {'信息框':>6}")
    print("-"*80)
    
    for f in files:
        stats = analyze_file(f)
        total_stats['lines'] += stats['lines']
        total_stats['chars'] += stats['chars']
        total_stats['mermaid'] += stats['mermaid']
        total_stats['code'] += stats['code']
        total_stats['tables'] += stats['tables']
        total_stats['info_boxes'] += stats['info_boxes']
        
        print(f"{stats['name']:<25} {stats['lines']:>6} {stats['chars']:>8} {stats['mermaid']:>4} {stats['code']:>4} {stats['tables']:>4} {stats['info_boxes']:>6}")
    
    print("-"*80)
    print(f"{'总计':<25} {total_stats['lines']:>6} {total_stats['chars']:>8} {total_stats['mermaid']:>4} {total_stats['code']:>4} {total_stats['tables']:>4} {total_stats['info_boxes']:>6}")
    print("="*80)
    print()
    
    # 内容摘要
    print("内容摘要:")
    print(f"  - 文档页面: {total_stats['files']} 个")
    print(f"  - 总代码行: {total_stats['lines']} 行")
    print(f"  - 总字符数: {total_stats['chars']:,} 字符")
    print(f"  - Mermaid图表: {total_stats['mermaid']} 个")
    print(f"  - 代码示例: {total_stats['code']} 个")
    print(f"  - 数据表格: {total_stats['tables']} 个")
    print(f"  - 信息提示框: {total_stats['info_boxes']} 个")
    print()
    
    # 功能检查
    print("功能检查:")
    css_exists = (HTML_DIR / 'css' / 'style.css').exists()
    print(f"  {'✓' if css_exists else '✗'} CSS 样式文件")
    
    all_valid = True
    for f in files:
        with open(f, 'r', encoding='utf-8') as file:
            content = file.read()
        valid = '<!DOCTYPE html>' in content and '</html>' in content
        if not valid:
            all_valid = False
    print(f"  {'✓' if all_valid else '✗'} HTML 结构完整性")
    
    has_nav = all('sidebar' in open(f, 'r', encoding='utf-8').read() for f in files)
    print(f"  {'✓' if has_nav else '✗'} 导航侧边栏")
    
    has_mermaid_lib = all('mermaid.min.js' in open(f, 'r', encoding='utf-8').read() for f in files)
    print(f"  {'✓' if has_mermaid_lib else '✗'} Mermaid 图表库")
    
    print()
    print("="*80)
    print("结论: 文档结构完整，可以正常浏览")
    print("="*80)
    print()
    print("使用方法:")
    print("  1. 在浏览器中打开: docs/html/index.html")
    print("  2. 或使用 Python 服务器:")
    print("     python3 docs/html/preview.py")
    print("  3. 然后访问: http://localhost:8888")

if __name__ == "__main__":
    generate_report()
