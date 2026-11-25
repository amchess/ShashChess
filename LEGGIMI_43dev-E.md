# ShashChess 43-dev-F: A + C

Variante di **43-dev-F** (commit `510dec5`): i file di questa cartella sostituiscono gli omonimi di 43-dev-F.
Se si parte dal ramo **C** (`5b28d76`) ne cambiano solo cinque (vedi la tabella in fondo).
Non aggiunge opzioni UCI. Il motore si chiama `ShashChess 43-dev-F`; il bench (16 MB, 1 thread)
è **2545670** nodi (43-dev-F 2666431, A 2932436, C 2535183).

## Perché

Nelle prove di Peter (4096 posizioni a 1", 506 a 5", 325 a 90" con MultiPV 3) A e C migliorano
43-dev-F sulle posizioni tranquille e restano al suo livello sulla 325. Divise per tipo di radice
(flag calcolati da 43-dev-F a profondità 1, quindi indipendenti dai risultati), le due varianti
guadagnano in posizioni diverse del 4096:

| 4096 a 1", radice con... | Posizioni | A − 43-dev-F | C − 43-dev-F | C − A |
|---|---|---|---|---|
| pedone in 6ª/7ª | 490 | −7 | −9 | −2 |
| sacrificio, senza pedone in 6ª/7ª | 1089 | +3 | **+36** (p 0,02) | **+33** (p 0,03) |
| nessuno dei due, né re in pericolo | 2463 | **+48** (p 0,005) | +32 (p 0,17) | −16 |

(Le altre 54 posizioni hanno solo il re in pericolo.) A rende la zona Capablanca-Tal alla ricerca
di Stockfish e guadagna soprattutto dove la radice non ha flag; C usa lo stile dell'avversario nei
suoi nodi e guadagna anche sulle radici con un sacrificio, dove A non cambia nulla. Totali: 4096
43-dev-F 2793, A 2837, C 2850 (Stockfish 19 2851); 506 43-dev-F 370, A 374, C 378; 325 con MultiPV 3
43-dev-F 264, A 263, C 265. E unisce le due modifiche.

## Le modifiche

1. **Da A: la zona Capablanca-Tal (51–75) tiene il suo tipo**, cioè la ricerca di Stockfish, senza
   regole di Capablanca, Tal o Petrosian (`getResilientShashinRange()`). In 43-dev-F il rapporto di
   profondità calcolato alla radice era ≥ 1 dalla seconda iterazione, quindi questa zona veniva
   sempre cercata come Capablanca (regole strategiche per una posizione quieta e pari).
2. **Da C: lo stile appartiene a un lato.** Nei nodi in cui muove l'avversario del lato al tratto
   alla radice (semimosse dispari, mosse nulle comprese) la ricerca usa la vista dell'avversario:
   zona speculare (Tal ↔ Petrosian, Capablanca-Tal ↔ Capablanca-Petrosian) e flag statici con i due
   re scambiati. Il numero di mosse legali e il flag del sacrificio restano quelli della radice.
   Anche il bonus Shashin nell'ordinamento delle mosse e la scala delle LMR (`reduction()`)
   seguono il lato al tratto.

Insieme, nelle due zone miste:

| Zona della radice | Nodi del lato alla radice | Nodi dell'avversario |
|---|---|---|
| Capablanca-Tal (51–75) | ricerca di Stockfish (da A) | Low Petrosian (zona speculare Capablanca-Petrosian) |
| Capablanca-Petrosian (25–49) | Low Petrosian, come in 43-dev-F | ricerca di Stockfish (speculare Capablanca-Tal, da A) |

Restano invariati: classificatore delle zone, valutazione, flag della radice (pedone in 6ª/7ª,
sacrificio, re in pericolo) e il loro effetto su null move, ProbCut, futility e riduzioni.

## Controlli locali

- **Compila senza avvisi**; stesse opzioni UCI di 43-dev-F; bench 2545670.
- **E è esattamente A + C**: ricompilata con `-DSC43C_OFF` (l'interruttore diagnostico di C, che
  spegne la vista dell'avversario) dà il bench di A, 2932436.
- **Rispetto a C** cambiano solo il caso Capablanca-Tal in `shashin_manager.cpp` (copiato da A),
  la nota diagnostica in `shashin_manager.h`, il nome e gli script `universal`.
- **4 thread e MultiPV 3**: ricerche di 5" senza problemi.

Sono controlli di funzionamento, non di forza: la forza si misura con le suite di Peter e con il match.

## Come provarla

1. Branch `E` da `43-dev-F` con questi file (oppure da `C`, sostituendo i cinque file indicati).
2. Peter: prima il 4096 a 1", poi il 506 a 5" e la 325 a 90" con MultiPV 3. Ci si aspetta che E
   tenga il guadagno di A sulle radici senza flag e quello di C sulle radici con un sacrificio, e che
   sulla 325 resti al livello di 43-dev-F.
3. Se regge, match contro 42.3 a 25'+10" sul pool dei nugget, come quello di C.

## File

| File | Origine | Rispetto a C |
|---|---|---|
| `src/shashin/shashin_manager.cpp` | C + caso Capablanca-Tal di A | cambia |
| `src/shashin/shashin_manager.h` | C, nota diagnostica aggiornata | cambia |
| `src/misc.cpp` | nome `43-dev-F` | cambia |
| `src/universal.sh`, `src/universal.bat` | eseguibile `ShashChess43-dev-F-universal` | cambiano |
| `src/search.cpp`, `src/search.h` | C | uguali |
| `src/movepick.cpp` | C | uguale |
| `src/shashin/moveconfig.cpp`, `src/shashin/moveconfig.h` | C | uguali |
