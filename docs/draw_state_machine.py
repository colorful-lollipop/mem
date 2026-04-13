#!/usr/bin/env python3
"""
RpcClient State Machine Visualizer
生成客户端生命周期状态机的精美可视化图表
"""

import graphviz
from graphviz import Digraph


def create_state_machine():
    """创建完整的状态机图"""
    
    dot = Digraph(comment='RpcClient State Machine')
    dot.attr(rankdir='TB', size='20,20', dpi='150')
    dot.attr('node', fontname='Noto Sans CJK SC', shape='box', style='rounded,filled', fontsize='11')
    dot.attr('edge', fontname='Noto Sans CJK SC', fontsize='9')
    
    # 定义颜色主题
    colors = {
        'uninitialized': '#E8E8E8',  # 浅灰
        'active': '#90EE90',          # 浅绿
        'recovering': '#FFD700',      # 金色
        'cooldown': '#FFA500',        # 橙色
        'nosession': '#FF6B6B',       # 红色
        'idleclosed': '#87CEEB',      # 天蓝
        'closed': '#696969',          # 深灰
        'event': '#FFFFFF',           # 白色背景
    }
    
    # 添加状态节点
    states = {
        'Uninitialized': {'color': colors['uninitialized'], 'shape': 'ellipse'},
        'Active': {'color': colors['active'], 'shape': 'box', 'penwidth': '3'},
        'Recovering': {'color': colors['recovering'], 'shape': 'box'},
        'Cooldown': {'color': colors['cooldown'], 'shape': 'box'},
        'NoSession': {'color': colors['nosession'], 'shape': 'box'},
        'IdleClosed': {'color': colors['idleclosed'], 'shape': 'box'},
        'Closed': {'color': colors['closed'], 'shape': 'ellipse', 'fontcolor': 'white'},
    }
    
    for state, attrs in states.items():
        dot.node(state, **attrs)
    
    # 添加边 - 使用cluster分组
    
    # 1. 初始化路径
    dot.edge('Uninitialized', 'Recovering', 
             label='Init()\nBeginSessionOpen()', 
             color='black', penwidth='2')
    
    # 2. 正常恢复成功
    dot.edge('Recovering', 'Active', 
             label='OpenSession() 成功\nFinalizeSessionOpen()', 
             color='green', penwidth='2')
    
    # 3. Active -> Cooldown (EngineDeath + Restart策略delay>0)
    dot.edge('Active', 'Cooldown', 
             label='EngineDeath\nonEngineDeath=Restart(delay>0)\n• CloseLiveSession()\n• ResolveAllPending(Crashed)\n• StartRecovery(delayMs)', 
             color='red', style='dashed')
    
    # 4. Active -> Recovering (EngineDeath + Restart策略delay=0)
    dot.edge('Active', 'Recovering', 
             label='EngineDeath\nonEngineDeath=Restart(0)\n(立即恢复)', 
             color='red', style='dashed', constraint='false')
    
    # 5. Active -> NoSession (EngineDeath + Ignore)
    dot.edge('Active', 'NoSession', 
             label='EngineDeath\nonEngineDeath=Ignore\n或无策略', 
             color='red', style='dotted')
    
    # 6. Active -> Cooldown (ExecTimeout + Restart)
    dot.edge('Active', 'Cooldown', 
             label='ExecTimeout\nonFailure=Restart(delayMs)', 
             color='orange', style='dashed')
    
    # 7. Active -> IdleClosed (onIdle=IdleClose)
    dot.edge('Active', 'IdleClosed', 
             label='idle超时\nonIdle=IdleClose', 
             color='blue', style='dashed')
    
    # 8. Cooldown -> Recovering
    dot.edge('Cooldown', 'Recovering', 
             label='CooldownExpired\n或新请求触发', 
             color='gold', penwidth='2')
    
    # 9. Recovering -> NoSession (失败)
    dot.edge('Recovering', 'NoSession', 
             label='OpenSession() 失败\nHandleSessionOpenFailure()', 
             color='red')
    
    # 10. NoSession -> Recovering (外部触发)
    dot.edge('NoSession', 'Recovering', 
             label='RequestExternalRecovery()\n外部触发恢复', 
             color='purple', style='dashed', constraint='false')
    
    # 11. IdleClosed -> Recovering (新请求)
    dot.edge('IdleClosed', 'Recovering', 
             label='新请求\nEnsureLiveSession()', 
             color='green', penwidth='2')
    
    # 12. 所有状态 -> Closed (Shutdown)
    for state in ['Uninitialized', 'Active', 'Recovering', 'Cooldown', 'NoSession', 'IdleClosed']:
        dot.edge(state, 'Closed', 
                 label='Shutdown()', 
                 color='black', style='dotted', fontsize='8')
    
    # 添加图例
    with dot.subgraph(name='cluster_legend') as legend:
        legend.attr(label='图例', style='rounded', bgcolor='#F0F0F0')
        legend.node('legend_init', '初始/终态', shape='ellipse', fillcolor=colors['uninitialized'])
        legend.node('legend_normal', '正常运行', shape='box', fillcolor=colors['active'])
        legend.node('legend_recover', '恢复中', shape='box', fillcolor=colors['recovering'])
        legend.node('legend_wait', '等待中', shape='box', fillcolor=colors['cooldown'])
        legend.node('legend_error', '错误状态', shape='box', fillcolor=colors['nosession'])
        legend.node('legend_idle', '空闲关闭', shape='box', fillcolor=colors['idleclosed'])
    
    return dot


def create_detailed_cooldown_diagram():
    """创建 Cooldown 状态的详细流程图 """
    dot = Digraph(comment='Cooldown State Detail')
    dot.attr(rankdir='LR', size='14,10', dpi='150')
    dot.attr('node', fontname='Noto Sans CJK SC', shape='box', style='rounded,filled', fontsize='10')
    
    # 子图：Cooldown 状态的内部逻辑
    with dot.subgraph(name='cluster_cooldown') as c:
        c.attr(label='Cooldown 状态详细处理', style='rounded', bgcolor='#FFF8DC', color='#FFA500', penwidth='2')
        
        c.node('cooldown_entry', '进入 Cooldown\nStartRecovery(delayMs>0)', fillcolor='#FFA500')
        c.node('new_request', '新请求到达\nEnsureLiveSession()', fillcolor='#E8E8E8')
        c.node('check_cooldown', 'CooldownActive()?\n(now < cooldownUntilMs)', fillcolor='#FFE4B5', shape='diamond')
        c.node('wait_cooldown', 'WaitForCooldownToSettle()\n阻塞等待', fillcolor='#FFD700')
        c.node('cooldown_expired', 'Cooldown 结束', fillcolor='#90EE90')
        c.node('to_recovering', '进入 Recovering\n自动尝试恢复', fillcolor='#FFD700')
        
        c.edge('cooldown_entry', 'new_request', label='新请求', style='invis')
        c.edge('new_request', 'check_cooldown')
        c.edge('check_cooldown', 'wait_cooldown', label='是\n(仍在冷却)')
        c.edge('check_cooldown', 'cooldown_expired', label='否\n(冷却结束)')
        c.edge('wait_cooldown', 'check_cooldown', label='唤醒后检查', style='dashed')
        c.edge('cooldown_expired', 'to_recovering')
    
    return dot


def create_policy_matrix():
    """创建策略决策矩阵图"""
    dot = Digraph(comment='Policy Decision Matrix')
    dot.attr(rankdir='TB', size='16,12', dpi='150')
    dot.attr('node', fontname='Noto Sans CJK SC', shape='box', style='rounded,filled', fontsize='10')
    
    # 事件类型
    events = ['EngineDeath', 'ExecTimeout', 'IdleTimeout']
    
    # 策略决策 -> 目标状态
    decisions = [
        ('EngineDeath', 'Restart(delay>0)', 'Cooldown', '#FFA500'),
        ('EngineDeath', 'Restart(0)', 'Recovering', '#FFD700'),
        ('EngineDeath', 'Ignore/NoPolicy', 'NoSession', '#FF6B6B'),
        ('ExecTimeout', 'Restart(delay>0)', 'Cooldown', '#FFA500'),
        ('ExecTimeout', 'Restart(0)', 'Recovering', '#FFD700'),
        ('ExecTimeout', 'Ignore', 'Active(保持)', '#90EE90'),
        ('IdleTimeout', 'IdleClose', 'IdleClosed', '#87CEEB'),
        ('IdleTimeout', 'Ignore', 'Active(保持)', '#90EE90'),
    ]
    
    # 创建矩阵布局
    with dot.subgraph(name='cluster_policy') as p:
        p.attr(label='策略决策矩阵', style='rounded', bgcolor='#F5F5F5')
        
        # 标题行
        for i, event in enumerate(events):
            p.node(f'header_{i}', event, fillcolor='#D3D3D3', shape='box', penwidth='2')
        
        # 决策节点
        prev_nodes = {}
        for event, decision, target, color in decisions:
            node_id = f'{event}_{decision}'
            label = f'{decision}\n→ {target}'
            p.node(node_id, label, fillcolor=color)
            prev_nodes[event] = node_id
    
    return dot


def create_simplified_overview():
    """创建简化概览图 - 突出主要流程 """
    dot = Digraph(comment='State Machine Overview')
    dot.attr(rankdir='TB', size='12,10', dpi='150')
    dot.attr('node', fontname='Noto Sans CJK SC', shape='box', style='rounded,filled', fontsize='11', penwidth='2')
    dot.attr('edge', fontname='Noto Sans CJK SC', fontsize='10', penwidth='2')
    
    colors = {
        'init': '#E8E8E8',
        'active': '#90EE90',
        'transient': '#FFD700',
        'error': '#FF6B6B',
        'end': '#696969',
    }
    
    # 核心状态
    dot.node('Init', 'Uninitialized', fillcolor=colors['init'], shape='ellipse')
    dot.node('Active', 'Active\n(正常服务)', fillcolor=colors['active'])
    dot.node('Cooldown', 'Cooldown\n(冷却等待)', fillcolor='#FFA500')
    dot.node('Recovering', 'Recovering\n(恢复中)', fillcolor=colors['transient'])
    dot.node('NoSession', 'NoSession\n(无会话)', fillcolor=colors['error'])
    dot.node('Closed', 'Closed\n(已关闭)', fillcolor=colors['end'], fontcolor='white', shape='ellipse')
    
    # 核心流程
    dot.edge('Init', 'Recovering', label='初始化', color='black')
    dot.edge('Recovering', 'Active', label='连接成功', color='green')
    
    # 异常恢复循环
    with dot.subgraph(name='cluster_recovery') as r:
        r.attr(label='异常恢复循环', style='rounded', bgcolor='#FFF8DC', color='#FFA500')
        r.edge('Active', 'Cooldown', label='异常发生\n(EngineDeath/Timeout)', color='red', style='dashed')
        r.edge('Cooldown', 'Recovering', label='冷却结束', color='gold')
        r.edge('Recovering', 'Active', label='恢复成功', color='green')
        r.edge('Recovering', 'NoSession', label='恢复失败', color='red')
    
    # 关闭路径
    dot.edge('Active', 'Closed', label='Shutdown', style='dotted')
    dot.edge('NoSession', 'Closed', label='Shutdown', style='dotted')
    
    return dot


def main():
    """主函数：生成所有图表"""
    
    print("正在生成状态机图表...")
    
    # 1. 完整状态机图
    print("1. 生成完整状态机图...")
    sm = create_state_machine()
    sm.render('/root/code/demo/mem/docs/state_machine_full', format='png', cleanup=True)
    sm.render('/root/code/demo/mem/docs/state_machine_full', format='svg', cleanup=True)
    print("   ✓ state_machine_full.png/svg")
    
    # 2. Cooldown 详细流程
    print("2. 生成 Cooldown 详细流程图...")
    cd = create_detailed_cooldown_diagram()
    cd.render('/root/code/demo/mem/docs/state_machine_cooldown', format='png', cleanup=True)
    cd.render('/root/code/demo/mem/docs/state_machine_cooldown', format='svg', cleanup=True)
    print("   ✓ state_machine_cooldown.png/svg")
    
    # 3. 策略决策矩阵
    print("3. 生成策略决策矩阵图...")
    pm = create_policy_matrix()
    pm.render('/root/code/demo/mem/docs/state_machine_policy', format='png', cleanup=True)
    pm.render('/root/code/demo/mem/docs/state_machine_policy', format='svg', cleanup=True)
    print("   ✓ state_machine_policy.png/svg")
    
    # 4. 简化概览
    print("4. 生成简化概览图...")
    so = create_simplified_overview()
    so.render('/root/code/demo/mem/docs/state_machine_overview', format='png', cleanup=True)
    so.render('/root/code/demo/mem/docs/state_machine_overview', format='svg', cleanup=True)
    print("   ✓ state_machine_overview.png/svg")
    
    print("\n所有图表生成完成！")
    print("输出文件：")
    print("  - docs/state_machine_full.{png,svg}")
    print("  - docs/state_machine_cooldown.{png,svg}")
    print("  - docs/state_machine_policy.{png,svg}")
    print("  - docs/state_machine_overview.{png,svg}")


if __name__ == '__main__':
    main()
