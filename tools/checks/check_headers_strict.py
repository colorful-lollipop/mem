#!/usr/bin/env python3
"""
严格检查 .h 文件中变量命名是否符合 lowerCamelCase。
结构体字段也要求小驼峰，不支持下划线命名。
"""

import re
import sys
from pathlib import Path
from typing import List, Tuple, Set

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
    # 数字常量
    if re.match(r'^\d+[uUlLfF]?$', name):
        return True
    # 特定关键字
    if name in ['defined', 'offsetof', '__cplusplus', 'NULL', 'nullptr', 'true', 'false']:
        return True
    return False

def is_likely_type(name: str) -> bool:
    """检查是否是类型名"""
    type_suffixes = ['Callback', 'Handler', 'Traits', 'Policy', 'Config', 'Options', 
                     'View', 'Ptr', 'Ref', 'Iter', 'Iterator', 'Type', 'Tag',
                     'Exception', 'Error', 'Result', 'Status', 'State', 'Mode',
                     'Service', 'Client', 'Server', 'Manager', 'Factory', 'Builder',
                     'Proxy', 'Stub', 'Impl', 'Interface', 'Base', 'Derived', 'Executor']
    for suffix in type_suffixes:
        if name.endswith(suffix):
            return True
    return False

def find_struct_fields(content: str) -> List[Tuple[str, int, str]]:
    """查找结构体/类中的所有字段定义"""
    fields = []
    
    # 找到所有结构体/类的定义
    struct_pattern = r'(?:struct|class)\s+(\w+)\s*\{'
    for match in re.finditer(struct_pattern, content):
        struct_name = match.group(1)
        start_pos = match.end() - 1  # 指向 '{'
        
        # 找到匹配的 '}'
        brace_count = 0
        end_pos = start_pos
        for i in range(start_pos, len(content)):
            if content[i] == '{':
                brace_count += 1
            elif content[i] == '}':
                brace_count -= 1
                if brace_count == 0:
                    end_pos = i
                    break
        
        struct_body = content[start_pos:end_pos+1]
        
        # 解析结构体内的每一行
        lines = struct_body.split('\n')
        for line in lines:
            # 移除注释
            clean = re.sub(r'//.*$', '', line)
            
            # 匹配字段声明: Type fieldName;
            # 支持数组: Type fieldName[N];
            # 支持默认值: Type fieldName = value;
            field_pattern = r'(?:\w+(?:<[^>]+>)?)(?:\s*\*?\s*\*?)(?:\s+)([a-zA-Z_]\w*)(?:\s*\[\s*\d+\s*\])?\s*(?:=\s*[^;]+)?\s*;'
            for fmatch in re.finditer(field_pattern, clean):
                field_name = fmatch.group(1)
                if not is_valid_var_exception(field_name) and not is_lower_camel_case(field_name):
                    fields.append((field_name, clean.strip()))
    
    return fields

def main():
    print("=" * 80)
    print("头文件变量命名风格检查（严格模式）")
    print("检查规则: 所有变量（包括结构体字段）必须使用 lowerCamelCase (小驼峰)")
    print("=" * 80)
    print()
    
    base_dir = Path('/root/code/demo/mem')
    
    header_files = []
    for path in base_dir.rglob('*.h'):
        if any(skip in str(path) for skip in ['build', 'third_party', 'out', '.cache']):
            continue
        header_files.append(path)
    
    header_files.sort()
    print(f"找到 {len(header_files)} 个头文件需要检查\n")
    
    all_violations = []
    
    for i, hfile in enumerate(header_files, 1):
        try:
            with open(hfile, 'r') as f:
                content = f.read()
            lines = content.split('\n')
        except:
            continue
        
        file_violations = []
        
        # 检查每一行
        for line_num, line in enumerate(lines, 1):
            clean = re.sub(r'//.*$', '', line)
            
            # 跳过预处理器和特定模式
            if clean.strip().startswith('#'):
                continue
            if re.match(r'^\s*(?:using|typedef|namespace)\s+', clean):
                continue
            
            # 匹配变量/字段声明
            # 模式: Type name; 或 Type name = value; 或 Type name[N];
            var_patterns = [
                r'\b(?:static|constexpr|const|volatile|extern|mutable|inline)?\s*\w+(?:<[^>]+>)?(?:\s*\*?\s*\*?)(?:\s+)([a-zA-Z_]\w*)(?:\s*\[\s*\d+\s*\])?\s*(?:=\s*[^;]+)?\s*;',
            ]
            
            for pattern in var_patterns:
                for match in re.finditer(pattern, clean):
                    var_name = match.group(1)
                    
                    # 排除关键字和类型
                    if var_name in ['if', 'while', 'for', 'switch', 'return', 'sizeof', 'new', 'delete', 'const', 'volatile']:
                        continue
                    if is_likely_type(var_name):
                        continue
                    if is_valid_var_exception(var_name):
                        continue
                    if is_lower_camel_case(var_name):
                        continue
                    
                    file_violations.append((var_name, line_num, clean.strip()))
        
        # 去重
        seen = set()
        unique_violations = []
        for v in file_violations:
            key = (v[0], v[1])
            if key not in seen:
                seen.add(key)
                unique_violations.append(v)
        
        if unique_violations:
            rel_path = hfile.relative_to(base_dir)
            print(f"  {rel_path}")
            for name, line, ctx in unique_violations:
                print(f"    行 {line}: {name}")
                all_violations.append((str(rel_path), name, line))
    
    print()
    print("=" * 80)
    print("检查结果汇总")
    print("=" * 80)
    
    if not all_violations:
        print("✓ 所有变量命名都符合 lowerCamelCase 规范！")
        return 0
    
    print(f"\n发现 {len(all_violations)} 处变量命名不规范:")
    for file_path, name, line in sorted(all_violations):
        print(f"  {file_path}:{line}: {name}")
    
    return 1

if __name__ == '__main__':
    sys.exit(main())
