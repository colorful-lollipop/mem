#!/usr/bin/env python3
"""
VES 文档 HTML 验证工具
检查HTML结构、Mermaid图表语法等
"""

import os
import re
import sys
from pathlib import Path

HTML_DIR = Path(__file__).parent

def check_html_structure(filepath):
    """检查HTML文件基本结构"""
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    errors = []
    
    # 检查基本标签
    if '<!DOCTYPE html>' not in content:
        errors.append("缺少 DOCTYPE")
    if '<html' not in content or '</html>' not in content:
        errors.append("缺少 html 标签")
    if '<head>' not in content or '</head>' not in content:
        errors.append("缺少 head 标签")
    if '<body>' not in content or '</body>' not in content:
        errors.append("缺少 body 标签")
    
    # 检查Mermaid
    has_mermaid = 'class="mermaid"' in content
    mermaid_count = content.count('class="mermaid"')
    
    # 检查CSS链接
    has_css = 'css/style.css' in content
    
    # 检查标题
    title_match = re.search(r'<title>(.*?)</title>', content)
    title = title_match.group(1) if title_match else "无标题"
    
    return {
        'valid': len(errors) == 0,
        'errors': errors,
        'has_mermaid': has_mermaid,
        'mermaid_count': mermaid_count,
        'has_css': has_css,
        'title': title
    }

def check_mermaid_syntax(filepath):
    """检查Mermaid图表语法"""
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # 提取所有mermaid图表
    mermaid_blocks = re.findall(r'<div class="mermaid">(.*?)</div>', content, re.DOTALL)
    
    issues = []
    for i, block in enumerate(mermaid_blocks, 1):
        block = block.strip()
        
        # 检查常见的语法问题
        if 'sequenceDiagram' in block:
            # 检查序列图语法
            if '-->>' in block and not re.search(r'\w+-->>\w+:', block):
                issues.append(f"图表{i}: 序列图消息格式可能有问题")
        
        if 'flowchart' in block or 'graph TD' in block:
            # 检查流程图箭头
            arrows = re.findall(r'(-->|-->|\.->|==>)', block)
            if not arrows:
                issues.append(f"图表{i}: 流程图缺少箭头连接")
    
    return issues

def check_navigation_links(filepath, all_files):
    """检查导航链接"""
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # 提取所有href
    links = re.findall(r'href="([^"]+)"', content)
    
    broken = []
    for link in links:
        if link.startswith('http') or link.startswith('#'):
            continue
        if link == 'css/style.css':
            continue
        if link not in all_files:
            broken.append(link)
    
    return broken

def main():
    print("="*60)
    print("VES 文档 HTML 验证报告")
    print("="*60)
    
    html_files = [f.name for f in HTML_DIR.glob('*.html')]
    all_files = set(html_files)
    
    print(f"\n发现 {len(html_files)} 个 HTML 文件:\n")
    
    total_mermaid = 0
    errors_found = 0
    
    for html_file in sorted(html_files):
        filepath = HTML_DIR / html_file
        
        # 检查结构
        result = check_html_structure(filepath)
        
        # 检查Mermaid语法
        mermaid_issues = check_mermaid_syntax(filepath)
        
        # 检查链接
        broken_links = check_navigation_links(filepath, all_files)
        
        # 输出结果
        status = "✓" if result['valid'] and not mermaid_issues and not broken_links else "✗"
        print(f"{status} {html_file}")
        print(f"   标题: {result['title']}")
        print(f"   Mermaid图表: {result['mermaid_count']} 个")
        
        if result['errors']:
            print(f"   错误: {', '.join(result['errors'])}")
            errors_found += len(result['errors'])
        
        if mermaid_issues:
            print(f"   Mermaid警告: {mermaid_issues}")
        
        if broken_links:
            print(f"   损坏链接: {broken_links}")
            errors_found += len(broken_links)
        
        total_mermaid += result['mermaid_count']
        print()
    
    # 检查CSS
    css_file = HTML_DIR / 'css' / 'style.css'
    if css_file.exists():
        css_size = css_file.stat().st_size
        print(f"✓ CSS 样式文件: {css_size} bytes")
    else:
        print(f"✗ CSS 样式文件缺失!")
        errors_found += 1
    
    print("\n" + "="*60)
    print(f"总计: {len(html_files)} 个HTML文件, {total_mermaid} 个Mermaid图表")
    if errors_found == 0:
        print("状态: 所有检查通过! ✓")
    else:
        print(f"状态: 发现 {errors_found} 个问题")
    print("="*60)
    
    return errors_found == 0

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
