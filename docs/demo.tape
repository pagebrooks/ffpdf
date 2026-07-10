# VHS tape for the README demo GIF (https://github.com/charmbracelet/vhs).
# Regenerate from the repo root with `make demo` (needs vhs + ttyd + ffmpeg,
# pdftotext, and a built ./ffpdf). Deterministic by design: re-run whenever
# CLI output changes so the README demo never goes stale.
#
# Uses docs/demo-form.pdf, a tiny AcroForm with Address/City/State/Zip fields.

Output docs/demo.gif

Set FontSize 18
Set Width 1000
Set Height 740
Set Padding 20
Set WindowBar Colorful
Set Theme "Catppuccin Mocha"
Set TypingSpeed 40ms

Type "# 1. extract the form's fields to an FDF"
Enter
Enter
Sleep 500ms
Type "./ffpdf fdf-extract docs/demo-form.pdf > answers.fdf"
Enter
Sleep 300ms

Enter
Type "# 2. fill in the values with any editor"
Enter
Enter
Sleep 500ms
Type "vi answers.fdf"
Enter
Sleep 1.5s
Type@25ms "/V ()"
Sleep 300ms
Enter
Sleep 300ms
Type@25ms "3li"
Type@25ms "123 Main St"
Sleep 300ms
Escape
Sleep 200ms
Type@25ms "n3li"
Type@25ms "Springfield"
Sleep 300ms
Escape
Sleep 200ms
Type@25ms "n3li"
Type@25ms "IL"
Sleep 300ms
Escape
Sleep 200ms
Type@25ms "n3li"
Type@25ms "62704"
Sleep 300ms
Escape
Sleep 200ms
Type ":wq"
Enter
Sleep 800ms

Type "clear"
Enter
Type "# 3. fill the form; --flatten bakes the values in and removes the form"
Enter
Enter
Sleep 500ms
Type "./ffpdf fill --flatten docs/demo-form.pdf answers.fdf > filled.pdf"
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
