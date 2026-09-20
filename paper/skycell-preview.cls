%% skycell-preview.cls --- a stand-in for the Astronomy & Astrophysics class.
%%
%% This is NOT the A&A class. `aa.cls' is distributed by the journal
%% (https://www.aanda.org/for-authors) and cannot be redistributed here, so this
%% file provides just enough of its interface -- \institute, \inst, the
%% five-part \abstract, \keywords, \degr, \arcmin, \arcsec -- for paper.tex to
%% compile and be read locally. For submission, drop the official aa.cls in this
%% directory and run `make aa'; paper.tex needs no change.
\NeedsTeXFormat{LaTeX2e}
\ProvidesClass{skycell-preview}[2026/09/20 local stand-in for aa.cls]
\DeclareOption*{\PassOptionsToClass{\CurrentOption}{article}}
\ProcessOptions\relax
\LoadClass[twocolumn,a4paper,10pt]{article}

\RequirePackage[a4paper,left=2.1cm,right=2.1cm,top=2.4cm,bottom=2.4cm,columnsep=0.8cm]{geometry}
\RequirePackage{natbib}
\bibpunct{(}{)}{;}{a}{}{,}
\RequirePackage{graphicx}
\RequirePackage{amsmath}
\RequirePackage{url}

%% --- units and small conveniences the A&A class provides --------------------
\newcommand{\degr}{\hbox{$^\circ$}}
\newcommand{\arcmin}{\hbox{$^\prime$}}
\newcommand{\arcsec}{\hbox{$^{\prime\prime}$}}
\newcommand{\object}[1]{#1}
\providecommand{\email}[1]{\texttt{#1}}
\newcommand{\inst}[1]{\textsuperscript{#1}}
\renewcommand{\and}{\ and\ }
\newcommand{\titlerunning}[1]{}
\newcommand{\authorrunning}[1]{}

\gdef\@subtitle{}
\gdef\@institute{}
\gdef\@keywords{}
\gdef\@abstracttext{}
\newcommand{\subtitle}[1]{\gdef\@subtitle{#1}}
\newcommand{\institute}[1]{\gdef\@institute{#1}}
\newcommand{\keywords}[1]{\gdef\@keywords{#1}}

%% --- \abstract: either one group (like article) or A&A's five ---------------
\let\endabstract\relax
\def\abstract#1{\@ifnextchar\bgroup{\@abs@five{#1}}{\@abs@one{#1}}}
\long\def\@abs@one#1{\gdef\@abstracttext{#1}}
\long\def\@abs@five#1#2#3#4#5{\gdef\@abstracttext{%
  \textbf{Context.} #1\par\smallskip
  \textbf{Aims.} #2\par\smallskip
  \textbf{Methods.} #3\par\smallskip
  \textbf{Results.} #4\par\smallskip
  \textbf{Conclusions.} #5}}

%% --- title block ------------------------------------------------------------
\renewcommand{\maketitle}{%
  \twocolumn[{%
    \begin{center}
      {\LARGE\bfseries\@title\par}
      \ifx\@subtitle\@empty\else\vskip 0.4em{\large\itshape\@subtitle\par}\fi
      \vskip 1.2em
      {\large\@author\par}
      \vskip 0.8em
      {\footnotesize\@institute\par}
      \vskip 0.6em
      {\footnotesize\@date\par}
    \end{center}
    \vskip 1.2em
    \begingroup\small
      \noindent\textbf{ABSTRACT}\par\smallskip
      \@abstracttext\par
      \ifx\@keywords\@empty\else\smallskip\noindent\textbf{Key words.} \@keywords\par\fi
    \endgroup
    \vskip 1.8em
  }]%
  \thispagestyle{plain}
}

\newenvironment{acknowledgements}%
  {\par\medskip\small\noindent\textit{Acknowledgements.} }%
  {\par\medskip}

\renewcommand{\abstractname}{Abstract}
\setlength{\parindent}{1.2em}
\renewcommand{\bibsection}{\section*{References}}
\endinput
