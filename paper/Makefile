# The skycell paper for Astronomy and Computing, on the journal's elsarticle
# template (elsarticle.cls v3.3, in TeX Live).
#
# `make`            -> skycell.pdf
# `make submission` -> submission/: one self-contained .tex, the figure and the
#                      highlights, ready to upload
# `make figures`    -> regenerates fig_benchmark.pdf from the benchmark results

LATEX = pdflatex -interaction=nonstopmode -halt-on-error

all: skycell.pdf

skycell.pdf: skycell.tex content.tex refs.tex fig_benchmark.pdf
	$(LATEX) skycell.tex
	$(LATEX) skycell.tex
	$(LATEX) skycell.tex

submission: skycell.pdf
	python3 flatten.py
	cd submission && $(LATEX) skycell.tex && $(LATEX) skycell.tex && $(LATEX) skycell.tex

figures:
	python3 figures.py

clean:
	rm -f *.aux *.log *.out *.spl submission/*.aux submission/*.log submission/*.out submission/*.spl

.PHONY: all submission figures clean
