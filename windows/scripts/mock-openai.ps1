# 翻译后端 mock：
#   POST */chat/completions → OpenAI 风格 SSE 流式
#   GET  */models           → 模型列表（设置页 Fetch models 用）
# 测试用：给 TypeTide 一个可对接的本地端点。
# 用法: .\mock-openai.ps1 [-Port 8199] [-Reply "TRANSLATED_TEXT"]
param(
    [int]$Port = 8199,
    [string]$Reply = "MOCK_TRANSLATION_OK",
    [ValidateRange(1, 10000)]
    [int]$ModelCount = 3
)
$ErrorActionPreference = "Stop"
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:$Port/")
$listener.Start()
Write-Output "mock backend listening on http://127.0.0.1:$Port"
while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    $req = $ctx.Request
    $res = $ctx.Response
    $null = (New-Object System.IO.StreamReader($req.InputStream)).ReadToEnd()
    Write-Output "request: $($req.HttpMethod) $($req.Url.AbsolutePath)"
    $isOpenAI = $req.Url.AbsolutePath -like "*/chat/completions"
    $isModels = $req.Url.AbsolutePath -like "*/models"
    if ($isModels) {
        # 供设置页的 "Fetch models" 使用；回显鉴权头方便确认请求带上了 Key
        $auth = $req.Headers['Authorization']
        $names = @('mock-model', 'mock-model-large', 'mock-model-cheap')
        $models = @(for ($i = 0; $i -lt $ModelCount; ++$i) {
            @{ id = if ($i -lt $names.Count) { $names[$i] } else { 'mock-model-{0:D3}' -f ($i + 1) } }
        })
        $body = @{ object = 'list'; data = $models } | ConvertTo-Json -Depth 3 -Compress
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
        $res.StatusCode = 200
        $res.ContentType = "application/json"
        $res.Headers.Add("X-Mock-Authorization", [string]$auth)
        $res.OutputStream.Write($bytes, 0, $bytes.Length)
        $res.Close()
        continue
    }
    if (-not $isOpenAI) {
        $res.StatusCode = 404; $res.Close(); continue
    }
    $res.StatusCode = 200
    $res.ContentType = "text/event-stream"
    $res.SendChunked = $true
    $writer = New-Object System.IO.StreamWriter($res.OutputStream, (New-Object System.Text.UTF8Encoding($false)))  # 无 BOM
    # 把回复拆成两个 delta 模拟流式
    $half = [int][Math]::Ceiling($Reply.Length / 2)
    foreach ($part in @($Reply.Substring(0, $half), $Reply.Substring($half))) {
        $json = '{"choices":[{"delta":{"content":' + (ConvertTo-Json $part) + '}}]}'
        $writer.Write("data: $json`n`n")
        $writer.Flush()
        Start-Sleep -Milliseconds 60
    }
    $writer.Write("data: [DONE]`n`n")
    $writer.Flush()
    $res.Close()
}
