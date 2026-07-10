# VHS tape for the README demo GIF (https://github.com/charmbracelet/vhs).
# Regenerate from the repo root with `make demo` (needs vhs + ttyd + ffmpeg,
# jq, pdftotext, and a built ./ffpdf). Deterministic by design: re-run whenever
# CLI output changes so the README demo never goes stale.
#
# Uses docs/demo-form.pdf, a tiny AcroForm with Address/City/State/Zip fields.
# Shows the JSON-first loop: discover fields -> author values as JSON -> fill.

Output docs/demo.gif

Set FontSize 18
Set Width 1100
Set Height 740
Set Padding 20
Set WindowBar Colorful
Set Theme "Catppuccin Mocha"
Set TypingSpeed 40ms

Type "# 1. discover the form's fields as JSON"
Enter
Enter
Sleep 500ms
Type "./ffpdf fields docs/demo-form.pdf | jq -c '.fields[] | {name, type}'"
Enter
Sleep 2.5s

Enter
Type "# 2. author the answers as a JSON object"
Enter
Enter
Sleep 500ms
Type "vi values.json"
Enter
Sleep 1.2s
Type ":set paste"
Enter
Sleep 200ms
Type "i"
Type@30ms "{"
Enter
Type@30ms '  "Address": "123 Main St",'
Enter
Type@30ms '  "City":    "Springfield",'
Enter
Type@30ms '  "State":   "IL",'
Enter
Type@30ms '  "Zip":     "62704"'
Enter
Type@30ms "}"
Escape
Sleep 300ms
Type ":wq"
Enter
Sleep 800ms

Type "clear"
Enter
Type "# 3. fill it (JSON in, JSON out); --flatten bakes the values in"
Enter
Enter
Sleep 500ms
Type "./ffpdf fill --flatten --json -o filled.pdf docs/demo-form.pdf values.json 2>/dev/null"
Enter
Sleep 3.5s

Enter
Type "# 4. the values now live in the PDF itself"
Enter
Enter
Sleep 500ms
Type "pdftotext -layout filled.pdf -"
Enter
Sleep 5s
