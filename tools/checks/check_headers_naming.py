#!/usr/bin/env python3
"""
检查 .h 文件中函数命名是否符合 UpperCamelCase，变量是否符合 lowerCamelCase。
"""

import re
import sys
from pathlib import Path
from typing import List, Tuple

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

def is_lower_camel_case(name: str) -> bool:
    """检查是否符合 lowerCamelCase (小驼峰)"""
    if not name:
        return False
    if not name[0].islower():
        return False
    if '_' in name:
        return False
    if name.isupper():
        return False
    return True

def is_valid_func_exception(name: str) -> bool:
    """函数名例外情况"""
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

def is_valid_var_exception(name: str) -> bool:
    """变量名例外情况"""
    # 全大写常量
    if name.isupper():
        return True
    # 下划线开头（内部使用）
    if name.startswith('_'):
        return True
    # 单字母变量
    if len(name) == 1:
        return True
    # 特定宏相关
    if name in ['defined', 'offsetof']:
        return True
    return False

def extract_problematic_names(file_path: str) -> Tuple[List[Tuple[str, int, str]], List[Tuple[str, int, str]]]:
    """
    提取不规范的函数名和变量名
    返回: (不规范函数列表, 不规范变量列表)
    每个元素: (name, line, context)
    """
    bad_functions = []
    bad_variables = []
    
    try:
        with open(file_path, 'r') as f:
            content = f.read()
        lines = content.split('\n')
    except Exception as e:
        return bad_functions, bad_variables
    
    # 预处理：移除注释和字符串
    content_clean = re.sub(r'"[^"]*"', '"""', content)
    content_clean = re.sub(r"'[^']*'", "''", content_clean)
    content_clean = re.sub(r'/\*.*?\*/', '', content_clean, flags=re.DOTALL)
    content_clean_lines = content_clean.split('\n')
    
    for line_num, (line, clean_line) in enumerate(zip(lines, content_clean_lines), 1):
        # 移除行内注释
        clean_line = re.sub(r'//.*$', '', clean_line)
        
        # 跳过预处理器指令
        if clean_line.strip().startswith('#'):
            continue
        
        # ========== 检查函数声明 ==========
        # 匹配函数声明模式: 返回类型 函数名(参数);
        # 避免匹配宏定义、if/for/while等控制语句
        func_patterns = [
            # 普通函数声明: void Foo();
            r'\b(?:virtual|static|inline|explicit|const)?\s*\w+(?:<[^>]+>)?\s+(\w+)\s*\([^)]*\)\s*(?:const)?\s*(?:=\s*0)?\s*;',
            # 构造函数声明: ClassName();
            r'\b(\w+)\s*\([^)]*\)\s*(?:noexcept)?\s*(?:=\s*(?:default|delete))?\s*;',
            # 析构函数声明
            r'(~\w+)\s*\([^)]*\)\s*(?:=\s*(?:default|delete))?\s*;',
        ]
        
        for pattern in func_patterns:
            for match in re.finditer(pattern, clean_line):
                func_name = match.group(1)
                
                # 排除关键字
                if func_name in ['if', 'while', 'for', 'switch', 'return', 'sizeof', 'decltype', 'typeof']:
                    continue
                
                # 排除模板参数
                if func_name in ['const', 'volatile', 'static', 'virtual', 'inline', 'explicit']:
                    continue
                
                if not is_valid_func_exception(func_name):
                    if not is_upper_camel_case(func_name):
                        bad_functions.append((func_name, line_num, line.strip()))
        
        # ========== 检查变量声明 ==========
        # 匹配变量声明（非函数参数，非模板参数）
        var_patterns = [
            # 类/结构体成员变量: Type varName;
            r'(?:private|public|protected)\s*:\s*(?:static\s+)?(?:constexpr\s+)?(?:const\s+)?\w+(?:<[^>]+>)?\s+(\w+)\s*[;=]',
            # 普通变量声明: Type varName;
            r'\b(?:static|constexpr|const|volatile|extern|mutable)?\s*\w+(?:<[^>]+>)?(?:\s*\*)?\s+(\w+)\s*[;=]',
            # 成员变量初始化: Type varName_ = ...;
            r'\b(?:static\s+)?(?:constexpr\s+)?(?:const\s+)?\w+(?:<[^>]+>)?(?:\s*\*)?\s+(m?\w+)\s*[=;]',
        ]
        
        for pattern in var_patterns:
            for match in re.finditer(pattern, clean_line):
                var_name = match.group(1)
                
                # 排除关键字
                if var_name in ['if', 'while', 'for', 'switch', 'return', 'sizeof', 'new', 'delete']:
                    continue
                
                # 排除宏
                if var_name in ['defined', '__cplusplus']:
                    continue
                
                # 排除函数名（已经在上面处理了）
                if '(' in clean_line[clean_line.find(var_name) + len(var_name):clean_line.find(var_name) + len(var_name) + 10]:
                    continue
                
                if not is_valid_var_exception(var_name):
                    if not is_lower_camel_case(var_name):
                        bad_variables.append((var_name, line_num, line.strip()))
    
    # 去重
    bad_functions = list(dict.fromkeys(bad_functions))
    bad_variables = list(dict.fromkeys(bad_variables))
    
    return bad_functions, bad_variables

def main():
    print("=" * 80)
    print("头文件命名风格检查")
    print("检查规则:")
    print("  - 函数、方法必须使用 UpperCamelCase (大驼峰)")
    print("  - 变量必须使用 lowerCamelCase (小驼峰)")
    print("=" * 80)
    print()
    
    base_dir = Path('/root/code/demo/mem')
    
    # 查找所有.h文件（排除build目录和third_party）
    header_files = []
    for path in base_dir.rglob('*.h'):
        # 跳过构建目录和第三方目录
        if any(skip in str(path) for skip in ['build', 'third_party', 'out', '.cache']):
            continue
        header_files.append(path)
    
    header_files.sort()
    print(f"找到 {len(header_files)} 个头文件需要检查\n")
    
    all_func_violations = []
    all_var_violations = []
    
    for i, hfile in enumerate(header_files, 1):
        funcs, vars = extract_problematic_names(str(hfile))
        if funcs or vars:
            rel_path = hfile.relative_to(base_dir)
            print(f"  [{i}/{len(header_files)}] {rel_path}")
            if funcs:
                print(f"    不规范函数 ({len(funcs)}):")
                for name, line, ctx in funcs:
                    print(f"      行 {line}: {name}")
                    all_func_violations.append((str(rel_path), name, line, ctx))
            if vars:
                print(f"    不规范变量 ({len(vars)}):")
                for name, line, ctx in vars:
                    print(f"      行 {line}: {name}")
                    all_var_violations.append((str(rel_path), name, line, ctx))
    
    print()
    print("=" * 80)
    print("检查结果汇总")
    print("=" * 80)
    
    if not all_func_violations and not all_var_violations:
        print("✓ 所有命名都符合规范！")
        return 0
    
    if all_func_violations:
        print(f"\n发现 {len(all_func_violations)} 处函数命名不规范:")
        for file_path, name, line, ctx in sorted(all_func_violations):
            print(f"  {file_path}:{line}: {name}")
    
    if all_var_violations:
        print(f"\n发现 {len(all_var_violations)} 处变量命名不规范:")
        for file_path, name, line, ctx in sorted(all_var_violations):
            print(f"  {file_path}:{line}: {name}")
    
    print()
    return 1

if __name__ == '__main__':
    sys.exit(main())
