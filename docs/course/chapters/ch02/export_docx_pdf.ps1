param(
    [Parameter(Mandatory = $true)][string]$InputDocx,
    [Parameter(Mandatory = $true)][string]$OutputPdf,
    [Parameter(Mandatory = $true)][string]$StatusFile
)

$ErrorActionPreference = 'Stop'
$word = $null
$document = $null
try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $document = $word.Documents.Open($InputDocx, $false, $true)
    $document.ExportAsFixedFormat($OutputPdf, 17)
    [System.IO.File]::WriteAllText($StatusFile, "ok`n$OutputPdf", [System.Text.UTF8Encoding]::new($false))
}
catch {
    [System.IO.File]::WriteAllText($StatusFile, "error`n$($_.Exception.ToString())", [System.Text.UTF8Encoding]::new($false))
    exit 1
}
finally {
    if ($null -ne $document) {
        $document.Close($false)
    }
    if ($null -ne $word) {
        $word.Quit()
    }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
