# `make`     -> skycell-preview.pdf, using the stand-in class (always works)
# `make aa`  -> paper.pdf, using the journal's aa.cls (put it in this directory:
#               https://www.aanda.org/for-authors/latex-issues/latex-tools)
# `make figures` -> regenerates the figures from the benchmark results

LATEX = pdflatex -interaction=nonstopmode -halt-on-error

all: skycell-preview.pdf

skycell-preview.pdf: preview.tex body.tex refs.tex skycell-preview.cls fig_benchmark.pdf fig_covering.pdf
	$(LATEX) preview.tex
	$(LATEX) preview.tex
	$(LATEX) preview.tex
	mv preview.pdf $@

aa: paper.pdf

paper.pdf: paper.tex body.tex refs.tex fig_benchmark.pdf fig_covering.pdf
	@test -f aa.cls || { echo "aa.cls not found -- download it from aanda.org and put it here"; exit 1; }
	$(LATEX) paper.tex
	$(LATEX) paper.tex
	$(LATEX) paper.tex

figures:
	python3 figures.py

clean:
	rm -f *.aux *.log *.out *.toc preview.pdf

.PHONY: all aa figures clean
