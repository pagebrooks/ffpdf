# VHS tape for the mail-merge demo GIF (https://github.com/charmbracelet/vhs).
# Regenerate from the repo root with `make demo` (needs vhs + ttyd + ffmpeg,
# pdftotext, envsubst, and a built ./ffpdf).
#
# Shows the batch use case: one form + a CSV -> one filled PDF per record,
# using docs/demo-template.fdf and docs/demo-customers.csv.

Output docs/demo-batch.gif

Set FontSize 18
Set Width 1100
Set Height 740
Set Padding 20
Set WindowBar Colorful
Set Theme "Catppuccin Mocha"
Set TypingSpeed 40ms

Type "# mail merge: fill one form for every row of a CSV"
Enter
Enter
Sleep 500ms
Type "cat docs/demo-customers.csv"
Enter
Sleep 2.5s

Enter
Type "# the FDF from fdf-extract, with placeholders where the values go"
Enter
Enter
Sleep 500ms
Type "cat docs/demo-template.fdf"
Enter
Sleep 3s

Type "clear"
Enter
Type "# render one filled PDF per record"
Enter
Enter
Sleep 500ms
Type "mkdir -p out"
Enter
Sleep 300ms
Type "while IFS=, read -r ADDRESS CITY STATE ZIP; do"
Enter
Type "  export ADDRESS CITY STATE ZIP"
Enter
Type "  envsubst < docs/demo-template.fdf |"
Enter
Type '    ./ffpdf fill --flatten -o "out/$ZIP-$CITY.pdf" - docs/demo-form.pdf 2>/dev/null'
Enter
Type "done < docs/demo-customers.csv"
Enter
Sleep 1.5s

Enter
Type "ls out"
Enter
Sleep 2.5s

Enter
Type "# spot-check one: the values are baked into the PDF"
Enter
Enter
Sleep 500ms
Type "pdftotext -layout out/97201-Portland.pdf -"
Enter
Sleep 5s
