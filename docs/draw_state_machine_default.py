#!/usr/bin/env python3
"""
Generate a concise RpcClient lifecycle state machine image for the current
default VesClient configuration.

Outputs:
  - docs/state_machine_default_current.jpg
  - docs/state_machine_default_current.png
  - docs/state_machine_default_current.svg
"""

from html import escape
from pathlib import Path

from PIL import Image
from graphviz import Digraph


DOCS_DIR = Path(__file__).resolve().parent
OUTPUT_BASENAME = DOCS_DIR / "state_machine_default_current"
FONT_NAME = "Noto Sans CJK SC"


def state_label(title: str, subtitle: str, header_color: str, body_color: str = "#FFFFFF") -> str:
    return f"""<
<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="10" COLOR="#D7DEE7">
  <TR>
    <TD BGCOLOR="{header_color}">
      <FONT FACE="{FONT_NAME}" POINT-SIZE="16"><B>{title}</B></FONT>
    </TD>
  </TR>
  <TR>
    <TD BGCOLOR="{body_color}">
      <FONT FACE="{FONT_NAME}" POINT-SIZE="10">{subtitle}</FONT>
    </TD>
  </TR>
</TABLE>
>"""


def note_label(title: str, lines: list[str], bg_color: str = "#FFF7E8") -> str:
    body = "<BR ALIGN=\"LEFT\"/>".join(escape(line) for line in lines)
    return f"""<
<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="8" COLOR="#D7DEE7">
  <TR>
    <TD BGCOLOR="{bg_color}">
      <FONT FACE="{FONT_NAME}" POINT-SIZE="13"><B>{escape(title)}</B></FONT>
    </TD>
  </TR>
  <TR>
    <TD BGCOLOR="#FFFFFF" ALIGN="LEFT">
      <FONT FACE="{FONT_NAME}" POINT-SIZE="10">{body}</FONT>
    </TD>
  </TR>
</TABLE>
>"""


def legend_label(title: str, rows: list[tuple[str, str]], bg_color: str = "#EEF3FF") -> str:
    row_html = []
    for number, text in rows:
        row_html.append(
            f"""  <TR>
    <TD ALIGN="LEFT" BALIGN="LEFT" BGCOLOR="#FFFFFF" WIDTH="42">
      <FONT FACE="{FONT_NAME}" POINT-SIZE="10"><B>{escape(number)}</B></FONT>
    </TD>
    <TD ALIGN="LEFT" BALIGN="LEFT" BGCOLOR="#FFFFFF" WIDTH="360">
      <FONT FACE="{FONT_NAME}" POINT-SIZE="10">{escape(text)}</FONT>
    </TD>
  </TR>"""
        )
    rows_html = "\n".join(row_html)
    return f"""<
<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="8" COLOR="#D7DEE7">
  <TR>
    <TD COLSPAN="2" BGCOLOR="{bg_color}" ALIGN="CENTER">
      <FONT FACE="{FONT_NAME}" POINT-SIZE="13"><B>{escape(title)}</B></FONT>
    </TD>
  </TR>
{rows_html}
</TABLE>
>"""


def build_graph() -> Digraph:
    dot = Digraph("rpc_client_default_state_machine", engine="neato")
    dot.attr(
        label="RpcClient 生命周期状态机",
        labelloc="t",
        labeljust="c",
        fontname=FONT_NAME,
        fontsize="20",
        bgcolor="#F7F8FA",
        pad="0.35",
        overlap="false",
        outputorder="edgesfirst",
        splines="true",
    )
    dot.attr("node", shape="plain", margin="0", fontname=FONT_NAME)
    dot.attr(
        "edge",
        fontname=FONT_NAME,
        fontsize="10",
        color="#6B7280",
        arrowsize="0.8",
        penwidth="1.8",
    )

    positions = {
        "uninitialized": "0,4!",
        "recovering": "4.0,4!",
        "active": "9.0,4!",
        "cooldown": "14.4,4!",
        "closed": "18.2,4!",
        "idle_closed": "9.0,1.0!",
        "no_session": "4.0,1.0!",
        "legend": "9.0,-2.2!",
    }

    dot.node(
        "uninitialized",
        state_label("Uninitialized", "初始态，尚未建立会话", "#E5E7EB", "#FAFAFA"),
        pos=positions["uninitialized"],
        pin="true",
    )
    dot.node(
        "recovering",
        state_label("Recovering", "正在 OpenSession()", "#F8D86A", "#FFF9DB"),
        pos=positions["recovering"],
        pin="true",
    )
    dot.node(
        "active",
        state_label("Active", "正常服务，存在 live session", "#A7E3AE", "#F2FFF3"),
        pos=positions["active"],
        pin="true",
    )
    dot.node(
        "cooldown",
        state_label("Cooldown", "默认冷却 200ms，避免抖动拉起", "#FFB868", "#FFF3E3"),
        pos=positions["cooldown"],
        pin="true",
    )
    dot.node(
        "closed",
        state_label("Closed", "Shutdown 终态", "#4B5563", "#E5E7EB"),
        pos=positions["closed"],
        pin="true",
    )
    dot.node(
        "idle_closed",
        state_label("IdleClosed", "空闲关闭，等待新请求重新拉起", "#9FD7FF", "#EFF8FF"),
        pos=positions["idle_closed"],
        pin="true",
    )
    dot.node(
        "no_session",
        state_label("NoSession", "恢复失败或显式 Ignore 后无会话", "#FFB0B0", "#FFF1F1"),
        pos=positions["no_session"],
        pin="true",
    )
    dot.node(
        "legend",
        legend_label(
            "编号说明",
            [
                ("1", "Init"),
                ("2", "OpenSession 成功"),
                ("3", "OpenSession 失败"),
                ("4", "NewRequest / 外部恢复(delay = 0)，重试 OpenSession"),
                ("5", "外部恢复(delay > 0)"),
                ("6", "EngineDeath / ExecTimeout，默认 Restart(200ms)"),
                ("7", "冷却结束，开始恢复"),
                ("8", "idle >= 60000ms"),
                ("9", "IdleClosed 收到 NewRequest"),
                ("10", "Shutdown"),
            ],
        ),
        pos=positions["legend"],
        pin="true",
    )

    dot.edge("uninitialized", "recovering", label="1", color="#2563EB")
    dot.edge("recovering", "active", label="2", color="#059669")
    dot.edge(
        "recovering",
        "no_session",
        label="3",
        color="#DC2626",
        tailport="s",
        headport="n",
    )
    dot.edge(
        "no_session",
        "recovering",
        label="4",
        color="#7C3AED",
        style="dashed",
        tailport="n",
        headport="s",
    )
    dot.edge(
        "no_session",
        "cooldown",
        xlabel="5",
        color="#A855F7",
        style="dashed",
    )
    dot.edge(
        "active",
        "cooldown",
        xlabel="6",
        color="#EA580C",
    )
    dot.edge(
        "cooldown",
        "recovering",
        xlabel="7",
        color="#D97706",
    )
    dot.edge(
        "active",
        "idle_closed",
        label="8",
        color="#0284C7",
        tailport="s",
        headport="n",
    )
    dot.edge(
        "idle_closed",
        "recovering",
        label="9",
        color="#0284C7",
    )
    dot.edge(
        "active",
        "closed",
        xlabel="10",
        color="#9CA3AF",
        style="dotted",
    )

    return dot


def export_graph(dot: Digraph) -> None:
    png_path = Path(dot.render(str(OUTPUT_BASENAME), format="png", cleanup=True))
    dot.render(str(OUTPUT_BASENAME), format="svg", cleanup=True)

    image = Image.open(png_path).convert("RGB")
    image.save(str(OUTPUT_BASENAME.with_suffix(".jpg")), quality=95, subsampling=0)


def main() -> None:
    export_graph(build_graph())
    print(f"Generated {OUTPUT_BASENAME.with_suffix('.jpg')}")
    print(f"Generated {OUTPUT_BASENAME.with_suffix('.png')}")
    print(f"Generated {OUTPUT_BASENAME.with_suffix('.svg')}")


if __name__ == "__main__":
    main()
