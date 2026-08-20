param(
    [Parameter(Mandatory = $true)]
    [string]$Command,

    [int]$Port = 2121,
    [int]$WaitMilliseconds = 750
)

$client = [Net.Sockets.TcpClient]::new()
try {
    $client.ReceiveTimeout = 2000
    $client.SendTimeout = 2000
    $client.Connect('127.0.0.1', $Port)
    $stream = $client.GetStream()

    $payload = [Text.Encoding]::UTF8.GetBytes($Command + "`n")
    $stream.Write($payload, 0, $payload.Length)
    $stream.Flush()

    Start-Sleep -Milliseconds $WaitMilliseconds
    $buffer = [byte[]]::new(65536)
    $builder = [Text.StringBuilder]::new()
    while ($stream.DataAvailable) {
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) { break }
        [void]$builder.Append([Text.Encoding]::UTF8.GetString($buffer, 0, $read))
    }
    $builder.ToString()
}
finally {
    $client.Dispose()
}
