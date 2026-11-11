# Copia o index.html (painel) para dentro do firmware, no arquivo painel.h.
# Rode sempre que mudar o index.html e grave o ESP32 de novo.
# Jeito facil: dar dois cliques em atualizar_painel.bat (na mesma pasta).

$raiz   = Split-Path -Parent $PSScriptRoot
$origem = Join-Path $raiz 'index.html'
$destino = Join-Path $PSScriptRoot 'torno_lathevision_esp32\painel.h'

$html = [IO.File]::ReadAllText($origem, [Text.Encoding]::UTF8)
if ($html.Contains(')lvpainel"')) { throw 'O index.html contem )lvpainel" e quebraria o painel.h.' }

$conteudo = @"
// ARQUIVO GERADO AUTOMATICAMENTE a partir do index.html (pasta principal do projeto).
// Nao edite aqui: edite o index.html e rode esp32\atualizar_painel.bat.
#pragma once
#include <pgmspace.h>

const char PAINEL_HTML[] PROGMEM = R"lvpainel(
"@ + $html + @"
)lvpainel";
"@

[IO.File]::WriteAllText($destino, $conteudo, (New-Object Text.UTF8Encoding $false))
Write-Host "painel.h atualizado: $destino"
