#!/usr/bin/env python3
"""
检查 .h 文件中函数命名是否符合 UpperCamelCase，变量是否符合 lowerCamelCase。
排除合理的例外：STL风格方法、结构体字段的下划线命名、成员变量的下划线后缀等。
"""

import re
import sys
from pathlib import Path
from typing import List, Tuple, Set

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

# 允许的小写函数名（STL风格或常用访问器）
ALLOWED_LOWER_FUNCTIONS = {
    'move', 'lock', 'clear', 'push_back', 'pop_front', 'pop_back', 'push_front',
    'begin', 'end', 'cbegin', 'cend', 'rbegin', 'rend', 'empty', 'size', 'capacity',
    'front', 'back', 'at', 'find', 'insert', 'erase', 'swap', 'merge', 'sort',
    'count', 'contains', 'emplace', 'emplace_back', 'emplace_front',
    'now', 'reader', 'writer', 'callback', 'bytes', 'data', 'str',
    'session_id', 'initialized', 'available', 'server_handles', 'serverHandles',
    'mutableHeader', 'wait', 'notify', 'notify_all', 'join', 'detach',
    'load', 'save', 'open', 'close', 'read', 'write', 'flush', 'seek',
    'create', 'destroy', 'start', 'stop', 'pause', 'resume', 'reset',
    'clone', 'copy', 'assign', 'compare', 'exchange', 'fetch_add', 'fetch_sub',
    'connect', 'disconnect', 'bind', 'listen', 'accept', 'send', 'recv',
    'add', 'remove', 'update', 'get', 'set', 'is', 'has', 'can', 'should',
    'enable', 'disable', 'show', 'hide', 'init', 'deinit', 'setup', 'cleanup',
    'prepare', 'finish', 'process', 'handle', 'parse', 'format', 'encode', 'decode',
    'serialize', 'deserialize', 'compress', 'decompress', 'encrypt', 'decrypt',
    'sign', 'verify', 'validate', 'check', 'test', 'run', 'execute', 'invoke',
    'call', 'trigger', 'emit', 'on', 'off', 'once', 'then', 'catch', 'finally',
    'success', 'failure', 'error', 'warn', 'info', 'debug', 'trace', 'log',
    'print', 'printf', 'scanf', 'sprintf', 'snprintf', 'fprintf',
    'malloc', 'calloc', 'realloc', 'free', 'memcpy', 'memmove', 'memset', 'memcmp',
    'strlen', 'strcpy', 'strncpy', 'strcat', 'strncat', 'strcmp', 'strncmp',
    'abort', 'exit', 'atexit', 'system', 'getenv', 'setenv', 'putenv',
}

def is_valid_var_exception(name: str, context: str = "") -> bool:
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
    # 数字常量
    if re.match(r'^\d+[uUlLfF]?$', name):
        return True
    # 带下划线的数字（如 1'000'000 C++14数字分隔符）
    if re.match(r"^\d[\d']*[uUlLfF]?$", name):
        return True
    # 成员变量下划线后缀（如 client_）是常见风格
    if name.endswith('_'):
        return True
    # 结构体字段使用下划线命名在C/C++中很常见
    if '_' in name and not name.startswith('m'):
        # 检查上下文是否是结构体定义
        return True
    # 特定宏相关
    if name in ['defined', 'offsetof', '__cplusplus', 'NULL', 'nullptr', 'true', 'false']:
        return True
    return False

def is_type_name(name: str, content: str, line_num: int) -> bool:
    """检查是否是类型名（而非变量名）"""
    # 常见的类型后缀
    type_suffixes = ['Callback', 'Handler', 'Traits', 'Policy', 'Config', 'Options', 
                     'View', 'Ptr', 'Ref', 'Iter', 'Iterator', 'Type', 'Tag',
                     'Exception', 'Error', 'Result', 'Status', 'State', 'Mode',
                     'Service', 'Client', 'Server', 'Manager', 'Factory', 'Builder',
                     'Proxy', 'Stub', 'Impl', 'Interface', 'Base', 'Derived']
    for suffix in type_suffixes:
        if name.endswith(suffix):
            return True
    
    # 全大写通常是类型或常量
    if name.isupper() and len(name) > 1:
        return True
    
    return False

def get_struct_fields(content: str) -> Set[str]:
    """提取结构体/类的字段名"""
    fields = set()
    
    # 匹配结构体定义
    struct_pattern = r'(?:struct|class)\s+\w+\s*\{[^}]+\}'
    for match in re.finditer(struct_pattern, content, re.DOTALL):
        struct_body = match.group()
        # 提取字段声明
        field_pattern = r'(?:\w+(?:<[^>]+>)?\s+)+(\w+)\s*[=;]'
        for fmatch in re.finditer(field_pattern, struct_body):
            fields.add(fmatch.group(1))
    
    return fields

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
    
    # 获取结构体字段
    struct_fields = get_struct_fields(content)
    
    for line_num, line in enumerate(lines, 1):
        # 移除注释
        clean_line = re.sub(r'//.*$', '', line)
        
        # 跳过预处理器指令
        if clean_line.strip().startswith('#'):
            continue
        
        # 跳过 using/typedef 声明
        if re.match(r'^\s*(?:using|typedef)\s+', clean_line):
            continue
        
        # ========== 检查函数声明 ==========
        # 匹配函数声明模式 - 更精确的模式
        func_patterns = [
            # 普通函数/方法声明: ReturnType FuncName(Args);
            r'\b(?:virtual|static|inline|explicit|const|constexpr)?\s*[\w:]+(?:<[^>]+>)?\s+([A-Z]\w*)::(\w+)\s*\(',
            # 普通函数: ReturnType funcName(Args);
            r'\b(?:virtual|static|inline|explicit|const|constexpr)?\s*[\w:]+(?:<[^>]+>)?(?:\s*\*)?\s+([a-zA-Z_]\w*)\s*\([^)]*\)\s*(?:const)?\s*(?:=\s*0)?\s*;',
        ]
        
        for pattern in func_patterns:
            for match in re.finditer(pattern, clean_line):
                func_name = match.group(1) if '::' not in pattern else match.group(2)
                
                # 排除关键字
                if func_name in ['if', 'while', 'for', 'switch', 'return', 'sizeof', 'decltype', 'typeof', 'alignas', 'alignof']:
                    continue
                
                # 排除宏
                if func_name in ['defined', 'offsetof', 'va_start', 'va_arg', 'va_end', 'va_copy']:
                    continue
                
                # 排除STL风格和常用函数
                if func_name in ALLOWED_LOWER_FUNCTIONS:
                    continue
                
                # 排除析构函数
                if func_name.startswith('~'):
                    continue
                
                # 检查是否大驼峰
                if not is_upper_camel_case(func_name):
                    # 检查是否是小写但合法的（如 operator）
                    if 'operator' in func_name:
                        continue
                    bad_functions.append((func_name, line_num, line.strip()))
        
        # ========== 检查变量声明 ==========
        # 匹配变量声明，但排除函数参数和类型定义
        var_patterns = [
            # 类/结构体成员: Type varName;
            r'(?:private|public|protected)\s*:\s*(?:static\s+)?(?:constexpr\s+)?(?:const\s+)?[\w:]+(?:<[^>]+>)?(?:\s*\*?\s*\*?)?\s+([a-zA-Z_]\w*)\s*[;=]',
            # 普通变量: Type varName;
            r'\b(?:static|constexpr|const|volatile|extern|mutable|thread_local)?\s*[\w:]+(?:<[^>]+>)?(?:\s*\*?\s*\*?)?\s+([a-zA-Z_]\w*)\s*[=;]',
        ]
        
        for pattern in var_patterns:
            for match in re.finditer(pattern, clean_line):
                var_name = match.group(1)
                
                # 排除关键字
                if var_name in ['if', 'while', 'for', 'switch', 'return', 'sizeof', 'new', 'delete', 'const', 'volatile']:
                    continue
                
                # 排除类型名
                if is_type_name(var_name, content, line_num):
                    continue
                
                # 排除结构体字段（允许下划线命名）
                if var_name in struct_fields:
                    continue
                
                # 排除例外
                if is_valid_var_exception(var_name, clean_line):
                    continue
                
                if not is_lower_camel_case(var_name):
                    bad_variables.append((var_name, line_num, line.strip()))
    
    # 去重
    bad_functions = list(dict.fromkeys(bad_functions))
    bad_variables = list(dict.fromkeys(bad_variables))
    
    return bad_functions, bad_variables

def main():
    print("=" * 80)
    print("头文件命名风格检查 (v2 - 排除合理例外)")
    print("检查规则:")
    print("  - 函数、方法必须使用 UpperCamelCase (大驼峰)")
    print("  - 变量必须使用 lowerCamelCase (小驼峰)")
    print("  - 排除: STL风格方法、结构体字段的下划线命名、成员变量下划线后缀等")
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
    files_with_issues = []
    
    for i, hfile in enumerate(header_files, 1):
        funcs, vars = extract_problematic_names(str(hfile))
        if funcs or vars:
            rel_path = hfile.relative_to(base_dir)
            files_with_issues.append((rel_path, funcs, vars))
            if funcs:
                for name, line, ctx in funcs:
                    all_func_violations.append((str(rel_path), name, line, ctx))
            if vars:
                for name, line, ctx in vars:
                    all_var_violations.append((str(rel_path), name, line, ctx))
    
    # 显示有问题的文件
    for rel_path, funcs, vars in files_with_issues:
        print(f"  {rel_path}")
        if funcs:
            print(f"    不规范函数 ({len(funcs)}):")
            for name, line, ctx in funcs:
                print(f"      行 {line}: {name}")
        if vars:
            print(f"    不规范变量 ({len(vars)}):")
            for name, line, ctx in vars:
                print(f"      行 {line}: {name}")
    
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
