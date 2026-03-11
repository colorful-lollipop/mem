#!/usr/bin/env python3
"""
检查 vpsdemo 和 memrpc 代码中函数命名是否符合 UpperCamelCase 风格。
使用 compile_commands.json 获取准确的编译参数。
"""

import json
import os
import sys
import subprocess
from pathlib import Path
from typing import List, Tuple, Optional, Dict

# 命名风格检查函数
def is_upper_camel_case(name: str) -> bool:
    """检查是否符合 UpperCamelCase (大驼峰)"""
    if not name:
        return False
    if not name[0].isupper():
        return False
    if '_' in name and not name.startswith('~'):
        return False
    if name.isupper():
        return False
    return True


def is_valid_exception(name: str) -> bool:
    """检查是否是合法的例外情况"""
    if 'operator' in name:
        return True
    if name.startswith('~'):
        return True
    if name == 'main':
        return True
    if name.startswith('_'):
        return True
    if 'lambda' in name:
        return True
    return False


def extract_functions_with_clang(file_path: str, compile_cmd: str) -> List[Tuple[str, int, str]]:
    """
    使用 clang -Xclang -ast-dump 提取函数名
    返回: [(func_name, line, func_type), ...]
    """
    functions = []
    
    try:
        # 构建 clang 命令
        cmd_parts = compile_cmd.split()
        # 去掉 -o 和输出文件
        filtered = []
        skip = False
        for part in cmd_parts:
            if skip:
                skip = False
                continue
            if part == '-o':
                skip = True
                continue
            if part.endswith('.o'):
                continue
            if 'c++' in part and '/usr/bin' in part:
                continue
            filtered.append(part)
        
        # 使用 clang-check 或 clang 的 AST dump
        cmd = ['clang', '-Xclang', '-ast-dump', '-fsyntax-only', '-fno-color-diagnostics'] + filtered + [file_path]
        
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        output = result.stderr if result.stderr else result.stdout
        
        # 解析 AST dump 中的函数声明
        for line in output.split('\n'):
            # 匹配函数声明模式
            # 例如: |-FunctionDecl 0x... <line:31:1, line:35:1> line:31:10 used MonotonicNowMs 'uint32_t (void)'
            if 'FunctionDecl' in line or 'CXXMethodDecl' in line or 'CXXConstructor' in line or 'CXXDestructor' in line:
                # 提取行号
                line_match = None
                if 'line:' in line:
                    try:
                        line_part = line.split('line:')[1].split(':')[0]
                        line_num = int(line_part)
                    except:
                        line_num = 0
                else:
                    line_num = 0
                
                # 提取函数名
                func_type = "unknown"
                if 'CXXMethodDecl' in line:
                    func_type = "method"
                elif 'CXXConstructor' in line:
                    func_type = "constructor"
                elif 'CXXDestructor' in line:
                    func_type = "destructor"
                elif 'FunctionDecl' in line:
                    func_type = "function"
                
                # 提取名称 - 在单引号之间
                if "'" in line:
                    name_part = line.split("'")[1] if "'" in line else ""
                    func_name = name_part.split('(')[0].strip()
                    
                    if func_name and not is_valid_exception(func_name):
                        if not is_upper_camel_case(func_name):
                            functions.append((func_name, line_num, func_type))
        
    except subprocess.TimeoutExpired:
        print(f"  Timeout parsing {file_path}")
    except Exception as e:
        print(f"  Error parsing {file_path}: {e}")
    
    return functions


def extract_functions_with_grep(file_path: str) -> List[Tuple[str, int, str]]:
    """
    使用正则表达式提取可能的函数定义
    作为后备方案
    """
    functions = []
    
    try:
        with open(file_path, 'r') as f:
            content = f.read()
        
        lines = content.split('\n')
        
        # 匹配函数定义的模式
        # 返回类型 函数名(参数) {
        patterns = [
            # 普通函数: void foo() {
            (r'^\s*(?:\w+::)?(\w+)\s*\([^)]*\)\s*(?:const)?\s*\{', 'function'),
            # 方法定义: void Class::foo() {
            (r'^\s*\w+\s+(\w+)::(\w+)\s*\(', 'method'),
        ]
        
        for line_num, line in enumerate(lines, 1):
            # 跳过注释
            if line.strip().startswith('//') or line.strip().startswith('*'):
                continue
            
            # 查找函数定义
            for pattern, func_type in patterns:
                match = re.search(pattern, line)
                if match:
                    func_name = match.group(1) if func_type == 'function' else match.group(2)
                    
                    if func_name and not is_valid_exception(func_name):
                        if not is_upper_camel_case(func_name):
                            functions.append((func_name, line_num, func_type))
                    break
        
    except Exception as e:
        print(f"  Error reading {file_path}: {e}")
    
    return functions


def main():
    print("=" * 80)
    print("函数命名风格检查工具 (使用 compile_commands.json)")
    print("检查规则: 函数、方法必须使用 UpperCamelCase (大驼峰)")
    print("=" * 80)
    print()
    
    base_dir = Path('/root/code/demo/mem')
    
    # 加载 compile_commands.json
    compile_commands = {}
    try:
        with open(base_dir / 'compile_commands.json', 'r') as f:
            commands = json.load(f)
        for entry in commands:
            file_path = os.path.abspath(entry['file'])
            compile_commands[file_path] = entry.get('command', '')
        print(f"加载了 {len(compile_commands)} 个编译命令")
    except Exception as e:
        print(f"Warning: 无法加载 compile_commands.json: {e}")
        return 1
    
    # 筛选需要检查的文件
    source_files = {}
    for file_path, cmd in compile_commands.items():
        # 只检查项目内的 vpsdemo 和 memrpc 代码
        if ('vpsdemo' in file_path or '/src/' in file_path or '/include/' in file_path):
            # 跳过测试文件和第三方代码
            if '/tests/' not in file_path and '/third_party/' not in file_path:
                source_files[file_path] = cmd
    
    print(f"找到 {len(source_files)} 个源文件需要检查")
    
    # 分类统计
    vpsdemo_files = {k: v for k, v in source_files.items() if 'vpsdemo' in k}
    memrpc_files = {k: v for k, v in source_files.items() if 'vpsdemo' not in k}
    
    print(f"  - vpsdemo: {len(vpsdemo_files)} 个文件")
    print(f"  - memrpc:  {len(memrpc_files)} 个文件")
    print()
    
    # 检查所有文件
    print("开始解析...")
    all_violations = []
    
    for i, (file_path, cmd) in enumerate(source_files.items(), 1):
        print(f"  [{i}/{len(source_files)}] {file_path}...", end=' ', flush=True)
        
        # 尝试使用 clang AST dump
        funcs = extract_functions_with_clang(file_path, cmd)
        
        if funcs:
            print(f"发现 {len(funcs)} 处不规范")
            for func_name, line, func_type in funcs:
                all_violations.append((file_path, func_name, line, func_type))
        else:
            print("OK")
    
    print()
    
    # 输出结果
    print("=" * 80)
    print("检查结果")
    print("=" * 80)
    
    if not all_violations:
        print("✓ 所有函数命名都符合 UpperCamelCase 规范！")
        return 0
    
    # 去重并按文件分组
    seen = set()
    unique_violations = []
    for v in all_violations:
        key = (v[0], v[1], v[2])
        if key not in seen:
            seen.add(key)
            unique_violations.append(v)
    
    # 按文件分组显示
    current_file = None
    vpsdemo_count = 0
    memrpc_count = 0
    
    for file_path, func_name, line, func_type in sorted(unique_violations):
        if file_path != current_file:
            print(f"\n文件: {file_path}")
            current_file = file_path
        print(f"  第 {line:4d} 行 | {func_type:12s} | {func_name}")
        
        if 'vpsdemo' in file_path:
            vpsdemo_count += 1
        else:
            memrpc_count += 1
    
    print()
    print("=" * 80)
    print(f"总计发现 {len(unique_violations)} 处命名不规范:")
    print(f"  - vpsdemo: {vpsdemo_count} 处")
    print(f"  - memrpc:  {memrpc_count} 处")
    print("=" * 80)
    
    return 1


if __name__ == '__main__':
    sys.exit(main())
