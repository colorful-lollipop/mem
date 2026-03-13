#!/usr/bin/env python3
"""
Strict header naming checker - ignores trailing underscore variables
Usage: python3 check_header_naming_strict.py [path/to/compile_commands.json]
"""

import json
import re
import sys
from pathlib import Path


def is_lower_camel_case(name):
    """Check if name is lowerCamelCase (no underscores)."""
    if not name or '_' in name or name[0].isupper():
        return False
    return True


def extract_violations(content, file_path):
    """Extract naming violations from header content."""
    violations = []
    lines = content.split('\n')
    
    # Patterns to exclude
    exclude_patterns = [
        r'^\s*#',  # Preprocessor directives
        r'^\s*//',  # Comments
        r'^\s*/\*',  # Block comment start
        r'^\s*\*',  # Block comment content
        r'^\s*namespace\s+\w+\s*=',  # Namespace aliases like "namespace MemRpc = ..."
        r'^\s*class\s+\w+\s*;',  # Forward declarations like "class TaskExecutor;"
        r'^\s*struct\s+\w+\s*;',  # Forward struct declarations
        r'^\s*using\s+',  # Type aliases
        r'^\s*typedef\s+',  # Typedefs
        r'^\s*template',  # Template lines
    ]
    
    # Patterns for variable declarations (struct/class fields, members, local vars)
    # Pattern: type varName; or type varName = value; or type varName[N];
    # Group 1: variable name
    var_pattern = re.compile(
        r'^\s*(?:const\s+|static\s+|constexpr\s+|mutable\s+)*'
        r'(?:[\w<>:]+(?:\s*<[^>]+>)?\s+)'
        r'(?:const\s+|volatile\s+)*'
        r'(\w+)\s*'
        r'(?:\[[^\]]*\]\s*)?(?:=\s*[^;]+)?;'
    )
    
    # Pattern for inline initialization in struct/class
    inline_pattern = re.compile(
        r'^\s*(?:[\w<>:]+(?:\s*<[^>]+>)?\s+)'
        r'(\w+)\s*=\s*[^;]+;'
    )
    
    # Function pointer pattern
    func_ptr_pattern = re.compile(
        r'^\s*(?:[\w<>:]+\s+)?'
        r'\(\s*\*\s*(\w+)\s*\)\s*\([^)]*\)'
    )
    
    for i, line in enumerate(lines, 1):
        # Skip excluded lines
        skip = False
        for pattern in exclude_patterns:
            if re.match(pattern, line):
                skip = True
                break
        if skip:
            continue
        
        # Skip lines with STL method calls like .begin(), .end(), .empty()
        if re.search(r'\.(begin|end|empty|rbegin|rend|size|clear|push_back)\s*\(', line):
            continue
        
        # Skip enum value declarations (ALL_CAPS)
        if re.match(r'^\s*[A-Z][A-Z0-9_]*\s*(?:=\s*\d+)?\s*,?', line):
            continue
        
        # Skip lines with template parameters (simplified check)
        if '<' in line and '>' in line and ',' in line.split('>')[0]:
            continue
        
        # Check for variable declarations
        matches = var_pattern.findall(line)
        matches += inline_pattern.findall(line)
        matches += func_ptr_pattern.findall(line)
        
        for var_name in matches:
            # Skip if it ends with _ (trailing underscore is allowed now)
            if var_name.endswith('_'):
                continue
            
            # Skip valid lowerCamelCase
            if is_lower_camel_case(var_name):
                continue
            
            # Skip ALL_CAPS constants and macros
            if var_name.isupper():
                continue
            
            # Skip common macro patterns like kXXX
            if re.match(r'^k[A-Z]', var_name):
                continue
            
            # Skip standard library types used as identifiers
            if var_name in ('size_t', 'ssize_t', 'nullptr', 'NULL', 'true', 'false'):
                continue
            
            # Skip single letters (like 'i', 'j')
            if len(var_name) == 1:
                continue
            
            # Skip template parameters
            if var_name in ('T', 'Type', 'Container', 'Allocator'):
                continue
            
            # Skip common type names
            if var_name in ('Iterator', 'iterator', 'const_iterator', 'value_type', 'key_type', 'mapped_type'):
                continue
            
            violations.append((i, var_name, line.strip()))
    
    return violations


def main():
    if len(sys.argv) > 1:
        compile_commands_path = Path(sys.argv[1])
    else:
        compile_commands_path = Path('/root/code/demo/mem/compile_commands.json')
    
    if not compile_commands_path.exists():
        print(f"Error: {compile_commands_path} not found")
        sys.exit(1)
    
    with open(compile_commands_path) as f:
        commands = json.load(f)
    
    # Collect header files
    header_files = set()
    for cmd in commands:
        file_path = Path(cmd['file'])
        if file_path.suffix in ('.h', '.hpp'):
            header_files.add(file_path)
        # Also add directory headers
        dir_path = file_path.parent
        for h in dir_path.glob('*.h'):
            header_files.add(h)
        for h in dir_path.glob('*.hpp'):
            header_files.add(h)
    
    # Also scan include directories
    include_dirs = [
        Path('/root/code/demo/mem/include'),
        Path('/root/code/demo/mem/demo/vpsdemo/include'),
    ]
    for d in include_dirs:
        if d.exists():
            for h in d.rglob('*.h'):
                header_files.add(h)
            for h in d.rglob('*.hpp'):
                header_files.add(h)
    
    # Convert to project-relative paths and filter
    project_root = Path('/root/code/demo/mem')
    filtered_headers = []
    for h in header_files:
        try:
            rel_path = h.relative_to(project_root)
            # Filter out third_party
            if 'third_party' not in str(rel_path):
                filtered_headers.append(h)
        except ValueError:
            continue
    
    # Check each header
    total_violations = 0
    files_with_violations = 0
    
    for header_path in sorted(filtered_headers):
        try:
            with open(header_path, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
        except Exception as e:
            continue
        
        violations = extract_violations(content, header_path)
        if violations:
            rel_path = header_path.relative_to(project_root)
            print(f"\n{rel_path}")
            for line_num, var_name, line_content in violations:
                print(f"  Line {line_num}: '{var_name}'")
                print(f"    {line_content[:100]}")
            total_violations += len(violations)
            files_with_violations += 1
    
    print(f"\n{'='*60}")
    print(f"Total: {total_violations} violations in {files_with_violations} files")
    print(f"(Ignoring trailing underscore variables)")
    print(f"{'='*60}")


if __name__ == '__main__':
    main()
