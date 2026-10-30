# Windows showcase media / Windows 展示素材

These README assets were captured from the current native Windows application, not from UI mockups or a test window. The surrounding **Commonplace** quote notebook is a small WPF editor used as the example application; it is not a TypeTide feature or bundled application.

README 展示素材来自当前原生 Windows 程序的实际运行画面，不是界面概念图或测试窗口。外围的 **Commonplace** 名言笔记本是演示用 WPF 编辑器，不属于 TypeTide 的功能或随附程序。

## What is recorded / 录制内容

- `translate.gif`: selecting a passage, invoking the normal read-shortcut action, selection capture, a real HTTP/SSE response, the loading shimmer, adaptive text fade-in, and the real Copy / Replace buttons.
- `rewrite.gif`: entering Chinese in the editor, invoking the normal rewrite-shortcut action, and the application's native text replacement after the response arrives.
- `icon.png`: the actual floating icon produced by dragging to select text in the editor.
- `settings-languages.png` and `settings-behavior.png`: the actual Languages and Behavior settings pages.

`translate.gif` 展示选区捕获、真实 HTTP/SSE 数据流、等待水波纹及自适应淡入；`rewrite.gif` 展示中文输入与应用实际执行的原地替换；其余截图分别为真实浮动图标、语言设置和行为设置。

## Quotations / 名言来源

- “We know what we are, but know not what we may be.” — William Shakespeare, *Hamlet*, Act IV, scene V. [Folger Shakespeare Library](https://www.folger.edu/explore/shakespeares-works/hamlet/read/4/5/).
- “To thine own self be true.” — William Shakespeare, *Hamlet*, Act I, scene III. [Folger Shakespeare Library](https://www.folger.edu/explore/shakespeares-works/hamlet/read/1/3/).
- “千里之行，始于足下。” — 老子，《道德经》第六十四章。展示英文译文： “A journey of a thousand miles begins with a single step.” [Chinese Text Project](https://ctext.org/dao-de-jing/zh).

## Capture and privacy / 录制与隐私

- The app used an isolated `TYPETIDE_DATA_DIR`, without accessing or replacing personal API settings or credentials.
- A loopback-only OpenAI-compatible endpoint supplied **preset quotation translations** through `/chat/completions` and SSE. These clips demonstrate the application's actual UI and workflows, **not live model generation, translation quality, or provider latency**.
- Recording automation selected text and invoked the application's normal shortcut-action handlers. The floating-icon screenshot used a real mouse-selection drag. No test harness or synthetic status text appears in the published media.
- Native window captures were restricted to the quote editor and TypeTide windows, placed at their actual relative coordinates. Desktop/chat content was excluded. Only cropping, GIF palette optimization, and encoding were applied; application controls and output were not painted over or recreated.
- GIF timing follows captured frame timestamps; unchanged frames can be coalesced without speeding up playback. The captures contain no API keys, private endpoints, personal paths, terminal output, or benchmark assertions.

使用独立配置与仅监听本机的 OpenAI 兼容演示端点，预设名言译文通过应用真实的流式翻译链路到达界面。录制只呈现指定编辑器和 TypeTide 窗口；未重绘或伪造控件，未展示测试文案，也未使用私人密钥、接口或个人路径。动图按录制时间戳播放，不将演示节奏冒充实际模型速度。
