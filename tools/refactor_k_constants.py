#!/usr/bin/env python3
"""
重构脚本: 将 kCamelCase 常量转换为 ALL_CAPS_WITH_UNDERSCORES 风格
符合项目 AGENTS.md 规范
"""

import re
import sys
import subprocess
from pathlib import Path

# 定义需要处理的目录
TARGET_DIRS = [
    "src",
    "include", 
    "demo",
    "tests",
    "third_party",
]

EXCLUDE_DIRS = [
    "build",
    ".git",
]

# 驼峰转下划线大写的核心函数
def camel_to_upper_snake(name):
    """kCamelCase -> ALL_CAPS_WITH_UNDERSCORES"""
    if not name.startswith('k'):
        return name
    
    # 去掉 k 前缀
    body = name[1:]
    
    # 处理连续大写（如 "ABCDef" -> "ABC_DEF"）
    result = []
    for i, char in enumerate(body):
        # 如果当前是大写，且下一个是小写，或前一个是小写，加下划线
        if char.isupper():
            if i > 0 and (body[i-1].islower() or 
                         (i < len(body)-1 and body[i+1].islower() and body[i-1].isupper())):
                result.append('_')
        result.append(char.upper())
    
    return ''.join(result)

# 查找 k 风格常量定义（仅匹配 constexpr/static const/const 开头的）
CONST_DEF_PATTERNS = [
    # constexpr type kName = value;
    r'(constexpr\s+\w+\s+)(k[A-Z][a-zA-Z0-9]*)\s*(=)',
    # constexpr const type* kName = value;
    r'(constexpr\s+const\s+\w+[\*\s]+)(k[A-Z][a-zA-Z0-9]*)\s*(=)',
    # static constexpr type kName = value;
    r'(static\s+constexpr\s+\w+\s+)(k[A-Z][a-zA-Z0-9]*)\s*(=)',
    # const char* kName = value;
    r'(const\s+\w+[\*\s]+)(k[A-Z][a-zA-Z0-9]*)\s*(=)',
    # const std::type kName = value;
    r'(const\s+std::\w+\s+)(k[A-Z][a-zA-Z0-9]*)\s*(=)',
]

def find_constant_definitions(content):
    """找出文件中所有的 k 风格常量定义"""
    constants = {}
    for pattern in CONST_DEF_PATTERNS:
        for match in re.finditer(pattern, content):
            full_match = match.group(0)
            k_name = match.group(2)
            new_name = camel_to_upper_snake(k_name)
            if k_name != new_name:
                constants[k_name] = new_name
    return constants

def replace_constant_in_content(content, constants):
    """替换内容中所有的常量定义和使用"""
    new_content = content
    for old_name, new_name in constants.items():
        # 使用单词边界匹配，避免部分匹配
        # 匹配 kName 作为完整标识符的情况
        pattern = r'\b' + re.escape(old_name) + r'\b'
        new_content = re.sub(pattern, new_name, new_content)
    return new_content

def get_target_files():
    """获取所有需要处理的 .cpp 和 .h 文件"""
    files = []
    root = Path('/root/code/demo/mem')
    
    for target_dir in TARGET_DIRS:
        dir_path = root / target_dir
        if not dir_path.exists():
            continue
        for ext in ['*.cpp', '*.h', '*.hpp']:
            for file_path in dir_path.rglob(ext):
                # 检查是否在排除目录中
                should_exclude = False
                for exclude in EXCLUDE_DIRS:
                    if exclude in str(file_path):
                        should_exclude = True
                        break
                if not should_exclude:
                    files.append(file_path)
    
    return sorted(files)

def dry_run():
    """试运行，只显示将要进行的修改"""
    print("=" * 60)
    print("DRY RUN - 将进行的修改：")
    print("=" * 60)
    
    total_replacements = 0
    files_with_changes = []
    
    for file_path in get_target_files():
        content = file_path.read_text()
        constants = find_constant_definitions(content)
        
        if constants:
            # 检查这些常量在整个文件中被使用了多少次
            new_content = replace_constant_in_content(content, constants)
            if new_content != content:
                count = sum(content.count(k) for k in constants.keys())
                total_replacements += count
                files_with_changes.append((file_path, constants, count))
                print(f"\n{file_path}")
                for old, new in constants.items():
                    print(f"  {old} -> {new}")
    
    print("\n" + "=" * 60)
    print(f"总计：{len(files_with_changes)} 个文件，约 {total_replacements} 处替换")
    print("=" * 60)
    return files_with_changes

def do_refactor(files_with_changes):
    """执行实际重构"""
    print("\n开始重构...")
    
    # 先备份（使用 git stash）
    result = subprocess.run(
        ['git', 'stash', 'push', '-m', 'refactor-backup'],
        cwd='/root/code/demo/mem',
        capture_output=True,
        text=True
    )
    if result.returncode != 0:
        print("警告：无法创建 stash 备份")
    
    modified_files = []
    
    for file_path, constants, _ in files_with_changes:
        content = file_path.read_text()
        new_content = replace_constant_in_content(content, constants)
        if new_content != content:
            file_path.write_text(new_content)
            modified_files.append(file_path)
            print(f"  ✓ {file_path}")
    
    print(f"\n已修改 {len(modified_files)} 个文件")
    return modified_files

def verify_build():
    """验证编译是否通过"""
    print("\n" + "=" * 60)
    print("验证编译...")
    print("=" * 60)
    
    # 清理并重新配置
    result = subprocess.run(
        ['cmake', '--build', 'build', '--target', 'clean'],
        cwd='/root/code/demo/mem',
        capture_output=True,
        text=True
    )
    
    result = subprocess.run(
        ['cmake', '--build', 'build'],
        cwd='/root/code/demo/mem',
        capture_output=True,
        text=True
    )
    
    if result.returncode == 0:
        print("✓ 编译成功")
        return True
    else:
        print("✗ 编译失败")
        print(result.stdout[-2000:] if len(result.stdout) > 2000 else result.stdout)
        print(result.stderr[-2000:] if len(result.stderr) > 2000 else result.stderr)
        return False

def main():
    if len(sys.argv) < 2:
        print("Usage: python refactor_k_constants.py <dry-run|apply>")
        print("")
        print("  dry-run  : 预览将要进行的修改")
        print("  apply    : 执行实际重构")
        sys.exit(1)
    
    command = sys.argv[1]
    
    if command == 'dry-run':
        dry_run()
    
    elif command == 'apply':
        files = dry_run()
        if not files:
            print("没有找到需要修改的常量")
            sys.exit(0)
        
        confirm = input("\n确认执行重构？此操作会修改文件 [y/N]: ")
        if confirm.lower() == 'y':
            do_refactor(files)
            if verify_build():
                print("\n✓ 重构完成且编译通过")
                print("建议运行测试: ctest --test-dir build --output-on-failure")
            else:
                print("\n✗ 编译失败，建议恢复:")
                print("  git checkout .")
                print("  git stash pop")
        else:
            print("已取消")
    
    else:
        print(f"未知命令: {command}")
        sys.exit(1)

if __name__ == '__main__':
    main()
