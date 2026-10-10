# `make`     -> skycell-preview.pdf, using the stand-in class (always works)
# `make aa`  -> paper.pdf, using the journal's aa.cls (put it in this directory:
#               https://www.aanda.org/for-authors/latex-issues/latex-tools)
# `make ac`  -> ac/skycell-ac.pdf, the Astronomy and Computing manuscript
#               (elsarticle.cls, in TeX Live)
# `make ac-submission` -> ac/submission/, the self-contained files to upload
# `make figures` -> regenerates the figures from the benchmark results

LATEX = pdflatex -interaction=nonstopmode -halt-on-error

all: skycell-preview.pdf

skycell-preview.pdf: preview.tex body.tex content.tex refs.tex skycell-preview.cls fig_benchmark.pdf fig_covering.pdf
	$(LATEX) preview.tex
	$(LATEX) preview.tex
	$(LATEX) preview.tex
	mv preview.pdf $@

aa: paper.pdf

paper.pdf: paper.tex body.tex content.tex refs.tex fig_benchmark.pdf fig_covering.pdf
	@test -f aa.cls || { echo "aa.cls not found -- download it from aanda.org and put it here"; exit 1; }
	$(LATEX) paper.tex
	$(LATEX) paper.tex
	$(LATEX) paper.tex

ac: ac/skycell-ac.pdf

ac/skycell-ac.pdf: ac/skycell-ac.tex content.tex refs.tex fig_benchmark.pdf
	cd ac && $(LATEX) skycell-ac.tex && $(LATEX) skycell-ac.tex && $(LATEX) skycell-ac.tex

# the files to upload: one self-contained .tex, the figure and the highlights
ac-submission: ac/skycell-ac.pdf
	python3 ac/flatten.py
	cd ac/submission && $(LATEX) skycell-ac.tex && $(LATEX) skycell-ac.tex && $(LATEX) skycell-ac.tex

figures:
	python3 figures.py

clean:
	rm -f *.aux *.log *.out *.toc preview.pdf ac/*.aux ac/*.log ac/*.out ac/submission/*.aux ac/submission/*.log ac/submission/*.out

.PHONY: all aa ac ac-submission figures clean
