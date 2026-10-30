# Fetch vendored third-party library sources (Phase-1 baseline).
# Uses gh-proxy.com mirror prefix; direct GitHub URLs are the canonical source.
$ErrorActionPreference = "Continue"
$MIRROR = "https://gh-proxy.com/"
$DEST = "$PSScriptRoot\..\third_party"
New-Item -ItemType Directory -Force -Path $DEST | Out-Null

# file, github url, [fallback urls...]
$items = @(
    @{ f = "lz4-1.10.0.tar.gz";        u = @("https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz") },
    @{ f = "zlib-1.3.2.tar.gz";        u = @("https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz") },
    @{ f = "zlib-ng-2.3.3.tar.gz";     u = @("https://github.com/zlib-ng/zlib-ng/archive/refs/tags/v2.3.3.tar.gz",
                                              "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz") },
    @{ f = "libdeflate-1.26.tar.gz";   u = @("https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.26.tar.gz") },
    @{ f = "brotli-1.2.1.tar.gz";      u = @("https://github.com/google/brotli/archive/refs/tags/v1.2.1.tar.gz",
                                              "https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz") },
    @{ f = "xz-5.8.4.tar.gz";          u = @("https://github.com/tukaani-project/xz/archive/refs/tags/v5.8.4.tar.gz") },
    @{ f = "snappy-1.3.1.tar.gz";     u = @("https://github.com/google/snappy/archive/refs/tags/1.3.1.tar.gz") },
    @{ f = "lzfse-1.0.tar.gz";         u = @("https://github.com/lzfse/lzfse/archive/refs/tags/1.0.tar.gz",
                                              "https://github.com/lzfse/lzfse/archive/heads/master.tar.gz") },
    @{ f = "bzip2-1.0.8.tar.gz";       u = @("https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz",
                                              "https://gh-proxy.com/https://gitlab.com/bzip2/bzip2/-/archive/bzip2-1.0.8/bzip2-bzip2-1.0.8.tar.gz") }
)

$fail = @()
foreach ($it in $items) {
    $out = Join-Path $DEST $it.f
    if ((Test-Path $out) -and (Get-Item $out).Length -gt 10000) { Write-Output "SKIP  $($it.f)"; continue }
    $done = $false
    foreach ($raw in $it.u) {
        $url = if ($raw -like "https://gh-proxy.com/*") { $raw } else { $MIRROR + $raw }
        Write-Output "GET   $($it.f)  <=  $url"
        curl.exe -sL --max-time 300 -o $out $url
        if ((Test-Path $out) -and (Get-Item $out).Length -gt 10000) { Write-Output "OK    $($it.f) ($([math]::Round((Get-Item $out).Length/1KB)) KB)"; $done = $true; break }
    }
    if (-not $done) { Write-Output "FAIL  $($it.f)"; $fail += $it.f }
}
if ($fail.Count -gt 0) { Write-Output "SUMMARY FAILED: $($fail -join ', ')" } else { Write-Output "SUMMARY: all fetched" }
