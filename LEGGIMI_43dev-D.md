# ShashChess 43-dev-F-D: la radice decide vicino alla radice

Variante di **43-dev-F** (commit `510dec5`): i file di questa cartella sostituiscono gli omonimi di 43-dev-F.
Non aggiunge opzioni UCI. Il motore si chiama `ShashChess 43-dev-F-D`; il bench (16 MB, 1 thread)
è **2421575** nodi (43-dev-F: 2666431).

## Il problema, misurato

In 42.3 e in 43-dev-F l'analisi Shashin della radice (zona WDL, re in pericolo, sacrificio possibile,
fortezza, pedone in 6ª/7ª) vale in **tutti i nodi** dell'albero: `search()` legge lo `staticState` della
radice. Se la radice ha un pedone in 6ª/7ª, un sacrificio o un re in pericolo, la null move salta in
tutto l'albero; se la radice è strategica, il ProbCut salta in tutto l'albero; e così via per razoring,
futility, soglie del movecount, LMR e riduzione dopo un miglioramento.

Nelle suite a 60 s (2.373 posizioni, server Intel):

| Radice con pedone in 6ª/7ª | Posizioni | Profondità 42.3 − Stockfish 19 | Risolte 42.3 | Risolte SF19 |
|---|---|---|---|---|
| 325, sì | 167 | −7,5 | 58,7% | 44,9% |
| 325, no | 158 | −2,6 | 69,0% | 65,8% |
| 2048, sì | 243 | −8,7 | 92,0% | 92,6% |
| 2048, no | 1805 | −2,3 | 94,0% | 93,4% |

Il salto della null move costa 6–9 semimosse, ma nelle posizioni con pedoni avanzati (zugzwang,
promozioni) è anche ciò che fa risolvere a ShashChess 23 posizioni più di Stockfish sulla 325.
In partita (match C, 1040 partite a 25'+10") ShashChess arriva in media a 1,3 semimosse meno di Stockfish.

## Le modifiche

1. **Orizzonte d'influenza della radice** (`ShashinParams::rootInfluenceHorizon = 8`, costante).
   Entro 8 semimosse dalla radice tutto resta come in 43-dev-F; oltre, le condizioni che dipendono
   dalla radice non toccano più potature e riduzioni, e la ricerca segue le regole di Stockfish 19:
   razoring, futility sul figlio, salti della null move, ProbCut (salto e profondità), ProbCut piccolo,
   soglie del movecount, offset LMR, formula del cutoffCnt, offset sulle mosse tranquille, fortezza
   nelle LMR, strato Shashin della riduzione dopo un miglioramento.
2. **Pedone vicino alla promozione valutato sul nodo.** Il salto della null move per un pedone in
   6ª/7ª si decide sulla posizione del nodo (due test di bitboard), a qualsiasi profondità: il rischio
   di zugzwang resta coperto dove c'è davvero, e sparisce dove il pedone non c'è più.
3. **Riduzione dopo un miglioramento di alpha.** Vicino alla radice resta lo strato Shashin, ma niente
   riduzione in più (+1) se la radice è complessa o tatticamente calda (re in pericolo, sacrificio,
   pedone vicino alla promozione): dopo il primo miglioramento le mosse successive possono ancora
   nascondere la confutazione. Oltre l'orizzonte, la riduzione base.
4. **Isteresi della zona** (`ShashinParams::zoneHysteresis = 2`). A ogni iterazione la radice viene
   riclassificata; un punteggio che oscilla di pochi centipedoni a cavallo di un confine (75/76, tra
   Capablanca-Tal e Low Tal, cade nel mezzo della fascia dei nugget) cambiava lo stile di tutta
   l'iterazione successiva. Ora la radice tiene la zona precedente finché la probabilità di vittoria
   resta entro 2 punti dalla sua fascia. Fra Capablanca e Caos totale (entrambi a 50, distinti dalla
   probabilità di patta) non c'è isteresi.

Restano invariati: classificatore delle zone, valutazione, ordinamento delle mosse, storici, estensioni
(compresa la singolare), finestre di aspirazione, rilevamento dei cicli, quiescenza.

## Da dove vengono

- **Misure nostre** (suite a 60 s, match C): il costo in profondità dei flag della radice e il loro
  beneficio sulle posizioni con pedoni avanzati.
- **Secondo parere** (Shashin Framework v2): l'orizzonte, il test di promozione sul nodo e l'idea
  dell'isteresi. Cambiato: l'orizzonte è una costante e non un'opzione UCI; vale per tutte le condizioni
  della radice su potature e riduzioni, non solo per cinque; l'isteresi è di 2 punti attorno ai confini
  della zona invece di 8 punti dall'ultima classificazione, perché 8 punti cancellerebbero le zone
  larghe 4–5 punti (Low Tal 76–79, Middle Low Tal 80–84, ...).
- **Terzo parere** (GuardedSearch): la cautela sulla riduzione dopo un miglioramento nelle radici
  calde. Non adottato il tetto di 1 semimossa solo alla terza mossa: si allontana da Stockfish 19
  nella direzione opposta e toglierebbe altra profondità a un motore che già cerca meno a fondo di
  Stockfish. Il suo applicatore non si applica a 42.3 né a 43-dev-F (il blocco è diverso).
- **Primo parere** (Framework 2.0): nessuna modifica. Il tempo di riflessione nel match C è identico
  per i due motori (32,1 s per mossa in media, mediana 22 s; mosse da 400 s o più: Stockfish 169,
  ShashChess 135), quindi la parte sulla gestione del tempo non trova riscontro.

## File

| File | Modifica |
|---|---|
| `src/search.cpp` | orizzonte su tutte le condizioni della radice, null move con test di promozione sul nodo, riduzione dopo un miglioramento |
| `src/shashin/shashin_params.h` | `rootInfluenceHorizon = 8`, `zoneHysteresis = 2` |
| `src/shashin/shashin_manager.h` | `inRootHorizon()`, `lastRawZone` |
| `src/shashin/shashin_manager.cpp` | fasce delle zone e isteresi in `setDynamicBaseState()` |
| `src/misc.cpp` | nome `43-dev-F-D` |
| `src/universal.sh`, `src/universal.bat` | eseguibile `ShashChess43-dev-F-D-universal` |

## Prove locali (1 thread, 64 MB, una macchina a 2 core)

- **Compila** senza avvisi; stesse opzioni UCI di 43-dev-F; bench 2421575.
- **Posizioni in cui 42.3 era molto meno profondo di Stockfish** (almeno 8 semimosse in meno a 60 s),
  a 10 s: senza pedoni avanzati, D arriva a 42,8 semimosse contro 36,7 di 43-dev-F (mediana +6); con
  pedoni avanzati 59,4 contro 58,1 (mediana +2), perché lì la null move resta spenta per scelta.
- **Suite 325 a 2 s:** stesse posizioni risolte (104 su 325 per entrambe, 20 scambi per parte),
  profondità +0,6 in media.
- **Pool dei nugget a 2 s:** stessa profondità (20,7). A tempi così brevi l'orizzonte di 8
  semimosse copre buona parte dell'albero; l'effetto è atteso a tempi lunghi.

Sono controlli di funzionamento, non di forza: la forza si misura con le suite a 60 s e con il match.

## Come provarla

1. Branch `D` da `43-dev-F`, con questi file.
2. Suite a 60 s, 5 passate (325 e le 193 di Peter), contro le passate già fatte di 42.3 e 43-dev-F:
   ci si aspetta più profondità; da controllare le posizioni con pedoni avanzati della 325.
3. Se regge, match contro 42.3 a 25'+10" sul pool dei nugget, come quello di C.

Per isolare l'orizzonte basta ricompilare con `rootInfluenceHorizon = 64` in `shashin_params.h`:
si ottiene 43-dev-F con le sole modifiche 2, 3 e 4.
