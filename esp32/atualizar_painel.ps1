# Copia o index.html (painel) para dentro dos firmwares:
#   - ESP32 DevKit: esp32\torno_lathevision_esp32\painel.h
#   - ESP32-C3 Mini: trecho entre as marcas no fim de esp32c3\lathevision_c3\lathevision_c3.ino
# Rode sempre que mudar o index.html e grave o ESP32 de novo.
# Jeito facil: dar dois cliques em atualizar_painel.bat (na mesma pasta).

$raiz    = Split-Path -Parent $PSScriptRoot
$origem  = Join-Path $raiz 'index.html'
$utf8    = New-Object Text.UTF8Encoding $false

$html = [IO.File]::ReadAllText($origem, [Text.Encoding]::UTF8)
if ($html.Contains(')lvpainel"')) { throw 'O index.html contem )lvpainel" e quebraria o firmware.' }
if ($html -match '(src|href)\s*=\s*"http://') { throw 'O index.html carrega algo por http:// (seria bloqueado em pagina https).' }

$bloco = 'const char PAINEL_HTML[] PROGMEM = R"lvpainel(' + "`n" + $html + "`n" + ')lvpainel";'

# ---------- ESP32 DevKit (painel.h) ----------
$destinoH = Join-Path $PSScriptRoot 'torno_lathevision_esp32\painel.h'
$conteudoH = "// ARQUIVO GERADO AUTOMATICAMENTE a partir do index.html (pasta principal do projeto).`n" +
             "// Nao edite aqui: edite o index.html e rode esp32\atualizar_painel.bat.`n" +
             "#pragma once`n#include <pgmspace.h>`n`n" + $bloco + "`n"
[IO.File]::WriteAllText($destinoH, $conteudoH, $utf8)
Write-Host "Atualizado: $destinoH"

# ---------- ESP32-C3 Mini (dentro do .ino) ----------
$destinoC3 = Join-Path $raiz 'esp32c3\lathevision_c3\lathevision_c3.ino'
if (Test-Path $destinoC3) {
  $ino = [IO.File]::ReadAllText($destinoC3, [Text.Encoding]::UTF8)
  $inicio = '// >>> INICIO DO PAINEL'
  $fim    = '// <<< FIM DO PAINEL'
  $i = $ino.IndexOf($inicio)
  $f = $ino.IndexOf($fim)
  if ($i -lt 0 -or $f -lt $i) { throw "Marcas do painel nao encontradas em $destinoC3" }
  $novo = $ino.Substring(0, $i + $inicio.Length) + "`n" + $bloco + "`n" + $ino.Substring($f)
  [IO.File]::WriteAllText($destinoC3, $novo, $utf8)
  Write-Host "Atualizado: $destinoC3"
}
