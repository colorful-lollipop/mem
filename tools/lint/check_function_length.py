#!/usr/bin/env python3
"""
C++ 函数行数检测工具 - AI 优化助手
检测超过指定行数的函数，生成 AI 友好的优化提示

用法:
    python3 check_function_length.py <path> [options]
    
示例:
    python3 check_function_length.py src/ --max-lines 50
    python3 check_function_length.py src/ --max-lines 50 --ai-prompt    # 生成 AI 优化提示
"""

import subprocess
import sys
import re
import argparse
from pathlib import Path
from typing import List, Dict, Tuple


def run_lizard(path: str, max_lines: int) -> List[Dict]:
    """运行 lizard 并解析输出"""
    cmd = ["lizard", path, "-L", str(max_lines), "-V"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        print("错误: 未找到 lizard。请安装: pip install lizard")
        sys.exit(1)
    
    functions = []
    # 解析格式: NLOC CCN token PARAM length location
    # 示例: 54      8    329      3      54 memrpc::DuplicateHandles@43-96@file.cpp
    pattern = r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(.+)@(\d+)-(\d+)@(.+)$'
    
    for line in result.stdout.strip().split('\n'):
        match = re.match(pattern, line)
        if match:
            nloc, ccn, tokens, params, length, func_name, start, end, file_path = match.groups()
            functions.append({
                'nloc': int(nloc),
                'ccn': int(ccn),
                'tokens': int(tokens),
                'params': int(params),
                'length': int(length),
                'name': func_name,
                'start_line': int(start),
                'end_line': int(end),
                'file': file_path
            })
    
    # 去重并只保留超过阈值的函数
    seen = set()
    unique = []
    for f in functions:
        key = (f['file'], f['name'], f['start_line'])
        if key not in seen and f['length'] > max_lines:
            seen.add(key)
            unique.append(f)
    return unique


def generate_ai_prompt(functions: List[Dict], max_lines: int) -> str:
    """生成 AI 优化提示"""
    if not functions:
        return f"✅ 所有函数都在 {max_lines} 行以内，代码规范良好！"
    
    lines = []
    lines.append(f"## 函数行数超标检测报告 (>{max_lines} 行)")
    lines.append(f"\n共发现 **{len(functions)}** 个函数需要优化：\n")
    
    # 按文件分组
    by_file: Dict[str, List[Dict]] = {}
    for func in functions:
        by_file.setdefault(func['file'], []).append(func)
    
    for file_path, funcs in sorted(by_file.items()):
        lines.append(f"\n### 📄 {file_path}")
        for func in funcs:
            lines.append(f"\n**{func['name']}** (行 {func['start_line']}-{func['end_line']})")
            lines.append(f"- 当前行数: `{func['length']}` 行 (超出 {func['length'] - max_lines} 行)")
            lines.append(f"- 圈复杂度: `{func['ccn']}`")
            lines.append(f"```cpp")
            lines.append(f"// 请优化此函数，目标: 不超过 {max_lines} 行")
            lines.append(f"// 建议: 提取子函数、减少嵌套、消除重复代码")
            lines.append(f"```")
    
    lines.append("\n---\n")
    lines.append("## 📝 AI 优化指令 (可复制给 AI)\n")
    lines.append("请帮我优化以下 C++ 函数，使其每个函数不超过 50 行。优化策略：")
    lines.append("1. 提取长逻辑为独立子函数")
    lines.append("2. 减少嵌套层级 (使用早返回)")
    lines.append("3. 合并重复代码块")
    lines.append("4. 简化复杂表达式\n")
    
    for file_path, funcs in sorted(by_file.items()):
        for func in funcs:
            lines.append(f"\n### {func['name']} in {file_path}:{func['start_line']}")
            lines.append(f"```cpp")
            lines.append(f"// 原函数 {func['length']} 行，需优化至 <=50 行")
            lines.append(f"```")
    
    return '\n'.join(lines)


def print_summary(functions: List[Dict], max_lines: int):
    """打印简洁摘要"""
    if not functions:
        print(f"✅ 所有函数都在 {max_lines} 行以内！")
        return
    
    print(f"\n⚠️  发现 {len(functions)} 个函数超过 {max_lines} 行:\n")
    print(f"{'行数':>6} | {'圈复杂度':>8} | {'函数名':<50} | {'文件'}")
    print("-" * 100)
    
    for func in sorted(functions, key=lambda x: -x['length']):
        print(f"{func['length']:>6} | {func['ccn']:>8} | {func['name']:<50} | {func['file']}:{func['start_line']}")


def main():
    parser = argparse.ArgumentParser(
        description='C++ 函数行数检测工具',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s src/                    # 检测 src/ 目录，默认 50 行阈值
  %(prog)s src/ --max-lines 30     # 设置 30 行阈值
  %(prog)s src/ --ai-prompt        # 生成 AI 优化提示
  %(prog)s src/ --json             # JSON 格式输出
        """
    )
    parser.add_argument('path', help='要检测的目录或文件')
    parser.add_argument('-n', '--max-lines', type=int, default=50, help='最大允许行数 (默认: 50)')
    parser.add_argument('--ai-prompt', action='store_true', help='生成 AI 优化提示')
    parser.add_argument('--json', action='store_true', help='JSON 格式输出')
    
    args = parser.parse_args()
    
    functions = run_lizard(args.path, args.max_lines)
    
    if args.json:
        import json
        print(json.dumps(functions, indent=2, ensure_ascii=False))
    elif args.ai_prompt:
        print(generate_ai_prompt(functions, args.max_lines))
    else:
        print_summary(functions, args.max_lines)
        if functions:
            print(f"\n💡 提示: 使用 --ai-prompt 生成 AI 优化指令")
            sys.exit(1)


if __name__ == '__main__':
    main()
