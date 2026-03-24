#!/usr/bin/env python3
"""分析代码规范问题：inline函数行数、圈复杂度、函数总行数"""

import re
import os
import glob
from pathlib import Path

def count_function_lines(content, start_line):
    """计算从start_line开始的函数行数（大括号匹配）"""
    lines = content.split('\n')
    brace_count = 0
    line_count = 0
    started = False
    
    for i in range(start_line - 1, len(lines)):
        line = lines[i]
        line_count += 1
        
        for char in line:
            if char == '{':
                brace_count += 1
                started = True
            elif char == '}':
                brace_count -= 1
                if started and brace_count == 0:
                    return line_count
    return line_count

def extract_inline_functions(content):
    """提取inline函数"""
    # 匹配 inline 函数定义 (简单匹配)
    pattern = r'inline\s+[\w:<>,\s&*]+\s+\w+\s*\([^)]*\)\s*\{'
    matches = []
    for match in re.finditer(pattern, content):
        line_num = content[:match.start()].count('\n') + 1
        func_lines = count_function_lines(content, line_num)
        # 提取函数名
        func_name = re.search(r'inline\s+[\w:<>,\s&*]+\s+(\w+)\s*\(', match.group()).group(1)
        matches.append((func_name, line_num, func_lines))
    return matches

def extract_function_definitions(content):
    """提取所有函数定义"""
    # 简单的函数定义匹配
    pattern = r'^[\w:<>,\s&*~]+\s+\w+::\w+\s*\([^)]*\)\s*\{|^\s*\w+\s+\w+\s*\([^)]*\)\s*\{'
    matches = []
    for match in re.finditer(pattern, content, re.MULTILINE):
        line_num = content[:match.start()].count('\n') + 1
        func_lines = count_function_lines(content, line_num)
        # 提取函数签名
        sig = match.group().strip()[:80]
        matches.append((sig, line_num, func_lines))
    return matches

def calculate_cyclomatic_complexity(content, start_line):
    """估算圈复杂度（简化版）"""
    lines = content.split('\n')
    brace_count = 0
    complexity = 1  # 基础复杂度
    started = False
    
    # 决策关键词
    decision_keywords = ['if', 'else if', 'for', 'while', 'case', 'catch', '&&', '||', '?']
    
    for i in range(start_line - 1, len(lines)):
        line = lines[i]
        
        for char in line:
            if char == '{':
                brace_count += 1
                started = True
            elif char == '}':
                brace_count -= 1
                if started and brace_count == 0:
                    return complexity
        
        if started:
            # 检查决策关键词
            stripped = line.strip()
            for kw in ['if ', 'else if ', 'for ', 'while ', 'case ', 'catch ']:
                if stripped.startswith(kw):
                    complexity += 1
                    break
            complexity += line.count('&&') + line.count('||')
            if '?' in line and ':' in line:
                complexity += 1
    
    return complexity

def analyze_file(filepath):
    """分析单个文件"""
    try:
        with open(filepath, 'r') as f:
            content = f.read()
    except Exception as e:
        return None
    
    issues = []
    
    # 检查inline函数
    inline_funcs = extract_inline_functions(content)
    for name, line, lines_count in inline_funcs:
        if lines_count > 10:
            issues.append({
                'type': 'inline_too_long',
                'name': name,
                'line': line,
                'lines': lines_count
            })
    
    return issues

def main():
    base_dirs = ['/root/mem/memrpc', '/root/mem/virus_executor_service']
    
    all_issues = []
    
    for base_dir in base_dirs:
        for root, dirs, files in os.walk(base_dir):
            for file in files:
                if file.endswith(('.h', '.hpp', '.c', '.cc', '.cpp')):
                    filepath = os.path.join(root, file)
                    issues = analyze_file(filepath)
                    if issues:
                        for issue in issues:
                            issue['file'] = filepath
                            all_issues.append(issue)
    
    # 输出结果
    print("=" * 80)
    print("代码规范分析报告")
    print("=" * 80)
    
    inline_issues = [i for i in all_issues if i['type'] == 'inline_too_long']
    if inline_issues:
        print(f"\n## Inline 函数超过10行的问题 ({len(inline_issues)}个)\n")
        for issue in inline_issues:
            rel_path = issue['file'].replace('/root/mem/', '')
            print(f"  {rel_path}:{issue['line']} {issue['name']}() - {issue['lines']}行")
    else:
        print("\n✓ 未发现inline函数超过10行的问题\n")
    
    print("\n" + "=" * 80)
    print("建议：")
    print("1. 将超过10行的inline函数改为非inline或在cpp文件中实现")
    print("2. 使用clang-tidy的 readability-function-cognitive-complexity 检查圈复杂度")
    print("3. 使用clang-tidy的 readability-function-size 检查函数大小")
    print("=" * 80)

if __name__ == '__main__':
    main()
