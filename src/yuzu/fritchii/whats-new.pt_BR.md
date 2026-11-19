## ⬇️ Baixe este arquivo: **`Fritchii-v6.2-Windows-x64.zip`**

Linux: **`Fritchii-v6.2-Linux-x86_64.AppImage`** (ou o `.tar.gz` da mesma compilação).

No Fritchii v4 até v6.1 você não precisa baixar nada: ele oferece esta atualização ao abrir. Caso contrário, extraia em qualquer pasta e execute `Fritchii-v6.2\fritchii.exe` (um pequeno `lichii.exe` permanece ao lado dele para que atalhos antigos continuem funcionando). Não baixe "Source code (zip)" nem "Source code (tar.gz)": eles não contêm o emulador.

---

Fritchii v6.2: placas de vídeo com pouca memória, PCs mais fracos e acima de 60 fps.

### Bom saber
- **Intel 12ª a 14ª geração (P-cores e E-cores): você não deve mais precisar de `EDEN_HYBRID_CORES=0`.** A v6.1 fixava rigidamente as threads de GPU nos P-cores, o que reduzia o fps pela metade em algumas dessas CPUs. A v6.2 deixa o Windows alocá-las por padrão. Se você adicionou essa opção, o Fritchii a desativa automaticamente na primeira inicialização (a linha permanece em Ferramentas > Parâmetros de inicialização como comentário). Se o fps ainda estiver em cerca de metade do esperado, remova o # na frente dela e nos avise no Discord.
- **A primeira inicialização pergunta uma vez: "Usar as configurações recomendadas?"** Usar recomendadas aplica as configurações ajustadas para o OoT e salva suas configurações antigas antes. Manter minhas configurações não altera nada. Você pode mudar depois em Configurações > Geral.
- Nenhuma nova preparação de shaders: a v6.2 mantém os shaders preparados da v6.1.

### Placas de vídeo com 4 a 6 GB
- **Sem mais descarte excessivo de texturas quando a memória de vídeo fica curta.** Em placas com pouca memória, o Fritchii ficava apagando texturas que o jogo estava prestes a usar novamente e recriando-as dezenas de vezes por quadro. Agora ele mantém aquelas às quais o jogo continua recorrendo. Em uma placa de 4 GB simulada em Castle Town: **43 → 75 fps**, 1% lows **20 → 57**. Uma placa de 4 GB real também precisa trocar memória via PCIe, então espere menos do que isso, mas ainda assim um grande avanço.
- Menos memória de vídeo desperdiçada em blocos de buffer meio vazios em placas menores.

### Imagem
- **O menu Imagem do OoT define o tamanho de renderização e a saída com upscaling separadamente**, permitindo renderizar em 720p e deixar o DLSS ou o FSR ampliar para 1080p ou mais.
- O DLSS agora tem um modelo **Automático**, definido como padrão. Ele usa o K, que é o modelo testado, e a barra de status mostra o modelo realmente em uso.

### Carregamento e áreas pesadas
- **Os carregamentos de área descompactam os arquivos do jogo nativamente** em vez de passarem pela CPU emulada, portanto as telas de carregamento devem ficar um pouco mais curtas.
- O jogo não começa mais a reorganizar sua memória de texturas a cada quadro, apenas uma vez por segundo. Cada textura que ele movia precisava ser configurada novamente pelo Fritchii.

### AMD
- **Nova configuração: Aceleração de oclusão do OoT** (Gráficos > Avançado). Em placas AMD, as verificações de visibilidade do OoT são respondidas sem as consultas lentas de GPU: cerca de **+9% de fps** no mercado em uma RX 9070 XT. Padrão vem ativado por padrão. Jogadores com AMD recebem uma oferta única da opção Completo, que vai ainda mais longe. NVIDIA e Intel já usam outro caminho rápido, então nada muda nelas.

### CPUs Intel
- **Configurações de CPU > P-cores da Intel:** em CPUs Intel com P-cores e E-cores, experimente a opção fixada e nos conte no Discord se ela ajuda no seu PC.

### Acima de 60 fps (Alta taxa de atualização)
- A passagem do tempo do dia não fica mais lenta nem congela acima de 60 fps.
- Epona salta cercas em vez de galopar através delas.
- As notas de uma música aparecem uma de cada vez, acompanhando a melodia.
- A teia da Deku Tree reage quando Link cai sobre ela, e plataformas móveis carregam Link corretamente.

### Abaixo de 60 fps
- O gorro de Link e outros tecidos se acomodam como deveriam quando o jogo roda lento, em vez de inflarem por um segundo.

### Estabilidade
- Quando o Windows fica sem memória por um momento, o Fritchii aguarda e tenta novamente em vez de fechar.
- Um relatório de travamento agora identifica a tarefa de GPU que travou, e alguns limites raros de GPU não encerram mais o jogo.

Algumas correções nesta versão são portes dos projetos Longshot e HeyListen, aos quais deixamos nossos agradecimentos.

---

## O que veio na v6.1

Fritchii v6.1: a correção da sala dos potes.

### Sala dos potes
- **Andar pela sala dos potes (guarita de Castle Town) não consome mais VRAM.** Mover-se ali criava milhares de novas imagens de sombra a cada poucos segundos e a memória de vídeo subia cerca de 4 GB. Já existia uma correção antes da v6.0, mas ela acabou ficando de fora do lançamento. Agora está integrada.
- No nosso PC de testes (andando lateralmente na sala dos potes, sem limite de fps): **67 → 71 fps**, 1% lows 46 → 48, memória de vídeo **10,5 → 7,4 GB**.

### Linux
- Os arquivos Linux da v6.0 foram compilados algumas horas antes da compilação final do Windows, então perderam as últimas correções da v6.0 e ainda exibiam "v6.0 Beta". A v6.1 no Linux traz tudo. As configurações ajustadas do Fritchii são aplicadas uma vez no Linux com esta atualização.

Nada mais muda: suas configurações permanecem como estão no Windows e não é necessária nenhuma nova preparação de shaders.

---

## Todas as novidades da v6.0

Se você está vindo da v5.x, isto é o que a v6.0 trouxe (e o que você também recebe agora).

A maior atualização do Fritchii até hoje: 840 alterações desde a v5.8.4. Ela tinha um objetivo, **manter o OoT em 60 fps**, e está sendo lançada porque conseguiu: o mercado de Castle Town agora roda nessa meta ou acima dela no nosso PC de testes, os engasgos longos praticamente sumiram, a sala dos potes não trava mais e as compilações para Linux chegaram.

**Atenção a três pontos únicos após atualizar:**
- **"Preparando shaders" roda mais uma vez, em todas as GPUs, e desta vez demora um pouco** (cerca de 15 minutos em uma CPU rápida de 8 núcleos, e mais tempo em CPUs de 6 núcleos e notebooks). Há dois motivos: a forma como os shaders são traduzidos mudou, então os shaders preparados na v5.x não são mais compatíveis, e **o programa agora se chama `fritchii.exe` em vez de `lichii.exe`**. O driver da sua placa de vídeo mantém um cache de shaders próprio por nome de programa, portanto para `fritchii.exe` ele começa vazio e precisa compilar tudo uma vez. Deixe concluir; as próximas inicializações voltam a ser rápidas.
- **O Fritchii pergunta uma vez se deseja limpar caches de shaders antigos** ("Libera X GB"). Limpar é seguro: são arquivos que o Fritchii reconstrói sozinho ou não lê mais, e os shaders serão preparados novamente de qualquer maneira. O cache próprio do driver da placa de vídeo nunca é tocado.
- **As configurações do OoT agora ficam em um único lugar.** Valores definidos nas próprias Propriedades do OoT (ou deixados lá por versões antigas do Lichii/Eden) costumavam sobrescrever silenciosamente o que você definia em Emulação > Configurar. Eles são movidos uma única vez para as configurações principais, exatamente como você os viu pela última vez no jogo, e uma breve mensagem lista o que foi movido. Um backup do arquivo antigo é feito antes (`custom\0400486024DEC000.ini.before-v6`). As Propriedades do OoT agora exibem apenas Complementos.

As configurações ajustadas do Fritchii são aplicadas mais uma vez com esta atualização; você pode alterar qualquer coisa depois.

#### Desempenho

Sem limite de fps no nosso PC de testes (Ryzen 7 7800X3D + RTX 4080, 1440p). Com o limite de 60 fps ativado você vê 60 cravados; o número sem limite mostra a folga restante. Cerca de 70 sem limite é o necessário para segurar 60 estáveis.
<!-- Medido no código do release candidate, 2026-11-18: mercado = PGO v10 A/B (inicializações limpas, últimos 60 s de 90), Zora's River = regressão da build 70, sala dos potes = builds 66-70. -->

| Local | v6.0 sem limite |
|---|---|
| Mercado de Castle Town (dia, cena mais pesada) | ~74 fps |
| Zora's River | ~91 fps |
| Sala dos potes (guarita) | ~92 fps |

Para comparação: na v5.8 o mercado rodava de 50 a 60 e raramente atingia 60.

- **Uma terceira thread de GPU.** Sua CPU faz muito trabalho antes de a placa de vídeo desenhar qualquer coisa: ela lê os comandos de GPU do jogo, prepara cada chamada de desenho e a registra para o Vulkan. No mercado, são cerca de 2.650 desenhos por quadro. Até a v5.8, duas threads dividiam esse trabalho e a que preparava os desenhos era o gargalo. A v6 adiciona uma terceira thread que registra os desenhos, distribuindo a carga por mais núcleos da CPU.
- **O código do próprio jogo roda mais rápido.** O código do OoT é recompilado para a sua CPU enquanto você joga. Esse recompilador recebeu uma rodada de melhorias: cerca de 11% menos tempo de CPU nas threads do próprio jogo (de 69,4 para 61,1 ms de tempo de núcleo por quadro no mercado). Ler o relógio do jogo não sai mais do código compilado (cerca de +2 fps).
- **Um único cache de código para todos os 6 núcleos de CPU emulados.** Antes, cada núcleo compilava e mantinha sua própria cópia do mesmo código do jogo. Agora eles compartilham um único cache: **cerca de 1 GB a menos de RAM** e aproximadamente um quarto a menos de engasgos ao carregar áreas.
- **A compilação foi otimizada novamente com base em jogo real.** O Fritchii é compilado com um perfil de como o código realmente executa no OoT. Retreinado no novo código, isso rendeu sozinho +6,5 fps no mercado (de 60,4 para 67,4, no mesmo código).
- **Menos esperas entre o jogo e a GPU.** Quando o jogo lê de volta algo produzido pela GPU, o emulador costumava esperar mais do que precisava. Várias dessas esperas foram eliminadas ou encurtadas. Maior ganho isolado: a fonte da Great Fairy passou de 52,3 para 71,0 fps.
- **Threads de GPU recebem núcleos próprios da CPU e prioridade mais alta,** para que o restante do emulador e o seu PC não as deixem em segundo plano (desativado automaticamente em CPUs com menos núcleos).
- **60 fps suaves em telas de 120/144 Hz.** Com o limite de 60 em uma tela de alta taxa de atualização, cerca de 1 quadro em cada 60 permanecia na tela por uma atualização extra (um engasgo de 25 ms). Agora 99,7 a 100% dos quadros são exibidos uniformemente, e o 1% low limitado subiu de 40,8 para 48,8 ou mais.
- **Sessões longas continuam rápidas.** Uma sessão de 40 minutos em Castle Town: mediana de 81,3 fps, 0 travamentos, nenhum engasgo acima de 50 ms após o primeiro minuto e RAM estável em cerca de 10,7 GB o tempo todo.

#### Engasgos e congelamentos

- **Engasgos ao abater inimigos cerca de metade menos frequentes.** Quando você derrota um inimigo, o jogo lê resultados da GPU de volta, e a cópia esperava atrás de todo o trabalho do quadro. Agora ela roda em sua própria fila de GPU logo após a tarefa que gravou os dados (verificada byte a byte). Em uma luta contra Stalchild com abates, quadros mais lentos que 20 ms caíram de 2 por segundo para 1, e o tempo de leitura de volta neles caiu de 10,6 para 5,1 ms. Eles ainda não desapareceram completamente (veja Problemas conhecidos).
- **Pop-in em vez de congelamentos de shader, em todas as GPUs.** Um shader que ainda está sendo compilado agora faz seu objeto aparecer um instante depois em vez de congelar o jogo. Antes, isso ocorria apenas em AMD e Intel. Os compute shaders usados na sua última sessão são compilados primeiro na inicialização, antes de o jogo solicitá-los.
- **Menos congelamentos na primeira execução de compute shaders** (zortos293/Lichii#2104): um passo de compute cujo shader ainda está sendo compilado é pulado naquele quadro quando nada lê seu resultado. Os maiores compute shaders alimentam a lógica do jogo e não podem ser pulados com segurança, então, após uma instalação nova ou atualização de driver, alguns congelamentos permanecem na primeira vez em que você os encontra; a partir da sessão seguinte, eles são compilados na inicialização.
- **Código do jogo que você já executou antes é compilado na inicialização.** O Fritchii lembra qual código do jogo suas sessões executaram e o compila antecipadamente na próxima vez, evitando compilação durante combates e áreas novas. Usar um mod de código (como o Hyrule Cheat Menu) não apaga mais essa lista.
- **Congelamentos mais curtos ao carregar áreas** (pelo cache de código compartilhado acima, além de menos texturas enviadas duas vezes).
- **Mercado à noite:** cães entrando por trás não causam mais engasgos, e galinhas muito menos.
- **Sala dos potes:** a sombra de cada objeto costumava precisar de uma nova imagem de profundidade quase a cada quadro (4.000 a 10.000 a cada 600 quadros ao andar lateralmente junto à parede). Os tamanhos das sombras agora são arredondados em degraus: de 30 a 90, com tempos de quadro caindo de 20,5–26 ms para 18,4–18,8 ms, mantendo a mesma aparência das sombras.
- **Um atraso na exibição não para mais o jogo.** Quando o Windows ou o driver seguravam um quadro, a emulação esperava junto.
- **Placas de 16 GB em 4K:** os limites de VRAM agora seguem o orçamento real do seu driver, então o jogo não transborda mais para a RAM do sistema causando congelamentos de segundos.
- **Placas de 8 GB e 6 GB** ativam automaticamente um modo para placas menores que mantém o jogo dentro da sua VRAM (em um teste com 8 GB: de 66,0 para 67,1 fps, 0,1% low de 38,5 para 42,6). Nessas placas, uma resolução acima de 1x roda em 1x.

#### Correções

##### Visual
- **Chega de flashes pretos ao trocar de item rapidamente:** uma imagem cuja memória de GPU era cedida a outra voltava vazia por um quadro. As imagens que precisam permanecer agora são lembradas entre sessões (e acompanham uma lista dos nossos testes), então nunca são removidas.
- **Lixo nos textos corrigido.** Caixas de diálogo, botões de escolha e números podiam ficar cobertos por lixo aleatório de textura, principalmente nas repetições da galeria de tiro e depois de ocultar e exibir a interface com o analógico esquerdo. O cache de texturas descartava as imagens de texto sem salvá-las antes; agora ele as salva (zortos293/Lichii#1310 tem os relatos).
- **Grama, pedrinhas e árvores distantes não somem mais enquanto Link balança a espada** (zortos293/Lichii#1628).
- **Blocos quadrados na névoa e na espuma ao redor de Link** (cachoeiras, rupees): um buffer de GPU lido pelo jogo estava vinculado com tamanho curto demais. Port do CheezyEmu, de CheezerWheezer.
- **O rastro de nado e as ondulações da água não dão mais saltos quando Link vira em taxas de quadros altas** (port do Blitchii #1180, encontrado e verificado por um testador).
- **Correções de alta taxa de atualização (Ctrl+U):** o som e a música não ficam mais mudos depois de algumas zonas de carregamento acima de 60 fps, as plataformas móveis funcionam acima de 100 fps, Gohma se direciona corretamente acima de 60, as vozes das cutscenes permanecem sincronizadas, a Scarecrow's Song mantém a duração, cabelo e tecido balançam na velocidade real, e o contador de rupees acima de Link volta a contar e a esmaecer (zortos293/Lichii#1660, #1663, #1679, #1743, #1745, #1793). Elas não se sobrepõem mais às flags que os cheat codes usam (#1981).
- **Imagem preta em alguns PCs portáteis no Linux** (quando uma das passagens de anti-aliasing do OoT não consegue ser compilada, o jogo agora desenha sem ela). Do CheezyEmu.

##### Falhas e congelamentos
- **Os crashes e congelamentos na sala dos potes (casa dos guardas) acabaram.** O pool de texturas do jogo se esgotava ali; o Fritchii agora o dobra com um patch integrado (zortos293/Lichii#1313), posicionado de modo que o mod Hyrule Cheat Menu continue funcionando.
- **Um crash raro de GPU na inicialização ("invalid write at 0x0") não pode mais acontecer:** o emulador não entrega mais um endereço de GPU vazio ao driver.
- **Ficar com pouca memória** não encerra mais o emulador durante a limpeza de memória em segundo plano (zortos293/Lichii#1451), e parar um jogo não termina mais em um "Userspace PANIC" (#1450, #1952).
- **O congelamento ao usar o warp da ocarina** (zortos293/Lichii#1218) e um crash em vídeos com o erro 2162-0001 (#1512).
- **O arquivo de cache de shaders não pode mais ser corrompido por um crash enquanto está sendo salvo.**
- **Arquivos de jogo danificados ou incomuns** são recusados com uma linha de log em vez de causar crash (um lote de correções do carregador vindas do Lichii).

##### Áudio e entrada
- **Fones Bluetooth permanecem em estéreo:** escolher "Nenhum" como microfone impede que o Windows os mude para o modo headset de baixa qualidade (Fritchii #60).
- **Iniciar com o volume da interface em 0** não deixa mais o jogo mudo até reiniciar (zortos293/Lichii#1847).
- **O giroscópio continua funcionando depois que um controle reconecta** (zortos293/Lichii#1805).

#### AMD

- **O Fritchii guarda seus shaders preparados por conta própria, como o driver da NVIDIA faz.** Os engasgos de 0,2-1 s na primeira vez que algo aparecia sumiram, e a preparação sobrevive às atualizações do Fritchii.
- **Flashes vermelhos sobre o chão molhado e a água** foram corrigidos. O driver da AMD interpola incorretamente um tipo de valor de espaço de tela quando um triângulo é cortado perto da câmera; a correção agora cobre também os mesh shaders, que é como o OoT desenha a maior parte das coisas.
- **Efeitos do Zora's Domain que eram ignorados voltam a ser desenhados** (uma sessão descartou 305.000 desses desenhos).
- **Chega de céu branco em Hyrule Field** (do CheezyEmu, via Lichii).
- **RX 470 a 590:** os shaders são compilados em 2 threads, então o PC não fica mais sem RAM depois da introdução (do CheezyEmu).
- **FSR 4 em placas RX 9000** é configurado como no CheezyEmu.

- **As configurações seguras são aplicadas automaticamente** no driver da AMD, sem precisar editar arquivos.
- **Texturas maiores não podem mais causar crash em placas AMD e Intel em 2160p e 2880p** em Castle Town (zortos293/Lichii#1890).

#### NVIDIA (DLSS)

- **O DLSS recebe jitter de câmera por padrão,** para detalhes mais nítidos. Se a imagem tremer para você, desmarque "Jitter do DLSS".
- **Nova configuração "Nitidez da textura"** (Automático, Mais nítida, Personalizada). O Automático agora se baseia no tamanho real de renderização do DLSS.
- **O DLSS e o FSR sempre usam a imagem própria do jogo,** já que escolhem o próprio tamanho de renderização. Antes, escolher uma Resolução mais alta os desativava silenciosamente ("O DLSS só funciona em 1x"). Os nomes de qualidade mostram os tamanhos reais do OoT.
- **Correções do DLSS vindas do Lichii:** entrada de cor limitada (clamp), câmera correta, escala de renderização e detalhe de textura (zortos293/Lichii#1672, #1659, #1589, #1690).
- O DLSS da NVIDIA tem prioridade sobre "Usar o DLSS do próprio jogo" quando ambos estão ativados. As opções do DLSS ficam ocultas em outras GPUs.

#### Intel

- **Correções para Intel Arc vindas de um fork da comunidade:** os shaders passam a ser preparados no Arc, além de correções para o modo como a GPU executa alguns shaders do OoT.
- **O Fritchii escolhe a sua Arc em vez dos gráficos integrados da CPU** automaticamente (agora ele escolhe a placa de vídeo com mais memória de vídeo e lembra a sua escolha pelo nome).
- **Uma compilação de shader não pode mais segurar um quadro por 10 s:** os desenhos esperam no máximo 50 ms por quadro e depois aparecem assim que o shader estiver pronto.
- A Arc funciona, mas ainda não perfeitamente (veja Problemas conhecidos).

#### Linux (novo)

- **Builds para Linux:** um AppImage e um tar.gz (x86_64). Execute `chmod +x Fritchii-v6.1-Linux-x86_64.AppImage` e rode-o, ou descompacte o tar.gz e execute `bin/lichii`. Seus dados ficam em `~/.local/share/fritchii`. O AppImage se atualiza sozinho, como a build do Windows.
- **Preparação de shaders em drivers Mesa** (RADV, ANV, NVK), assim como nos da NVIDIA.
- **NVIDIA DLSS no Linux** por meio do driver da NVIDIA (o painel do DLSS avisa se o seu driver for antigo demais).
- **Steam Deck:** inicia em modo portátil na primeira execução, e o limite do cache de shaders do Steam é aumentado para que a preparação persista. Funciona, mas, com cerca de 10-12 fps, fica abaixo do que o OoT precisa; uma Resolução menor (810p) ajuda.
- Crashes no Linux agora deixam um relatório no log, e há um formulário de issue para problemas no Linux.

#### Configurações e interface

- **Mais saídas de DLSS e FSR:** com um upscaler ativado, escolha 1080p, 1440p, 1800p, 4K, 2880p ou 8K (2880p exige 6 GB de memória de vídeo, 8K exige 12 GB; o menu oculta o que a sua placa não comporta). 4K e 1440p funcionam exatamente como antes. Saídas acima de 1440p partem da imagem em 1440p da Resolução máxima, então escolher uma delas ativa a Resolução máxima (o menu avisa). Os upscalers sempre partem da imagem própria do jogo (até 1080p, ou 1440p com a Resolução máxima), então, com um ativado, 1080p é o mínimo. Para ir abaixo de 1080p, defina Upscaler como Desativado e escolha uma Resolução.
- **Com DLSS ou FSR ativado, a linha diz "Escalar para"** e uma observação informa que o upscaler parte da imagem própria do jogo em 1080p (1440p com a Resolução máxima). Para ir abaixo de 1080p, defina Upscaler como Desativado e escolha uma Resolução menor.
- **A barra de status mostra a imagem real,** como "1440p → 4K · DLSS" ou "até 1080p · DLSS Nativo", e acompanha as mudanças de qualidade feitas dentro do jogo (antes dizia "Escala: 1x").
- **A Resolução máxima pergunta uma única vez:** a v6 desativa o complemento Resolução máxima e pergunta na primeira inicialização, informando o que ele exige (algo como uma RTX 3070 / RX 6800, 8 GB) e se a sua placa dá conta. Ele é necessário para as saídas de DLSS/FSR acima de 1440p.

- **"Bom saber" na primeira inicialização:** os problemas conhecidos que você pode encontrar, em uma única janela curta (Ajuda > Problemas conhecidos a mostra de novo).
- **Upscaler logo abaixo de Resolução:** uma linha com Jogo, FSR 3.1, FSR 4 e DLSS (apenas os que a sua GPU consegue executar).
- **Filtro da janela Automático (novo padrão):** uma imagem maior que a sua tela (1440p ou 4K em uma tela de 1080p) é reduzida com Area, que faz a média de cada pixel e não cintila; uma menor é esticada com ScaleForce, como antes.
- **A Resolução mostra resoluções reais,** como **1440p (Portátil 1080p)**, de 360p até 8K, para que PCs mais fracos possam escolher 810p ou 720p. Mudá-la prepara os shaders de novo uma vez, para que a próxima sessão não os compile durante o jogo.
- **Ferramentas > Parâmetros de inicialização:** veja e edite as chaves de teste (`nx2_switches.txt`) dentro do Fritchii, com Salvar e Reiniciar.
- **Limite "Ilimitado" da alta taxa de atualização:** com VSync ligado (Mailbox), a alta taxa de atualização pode passar da taxa do seu monitor. Não combine Ilimitado com VSync desligado: o jogo pode sofrer crash logo depois de uma tela de carregamento (a lista avisa).
- **Overlay de desempenho:** tempo de GPU e de CPU por quadro, o tempo de cada thread do emulador e o que está segurando a taxa de quadros ("Limitado por").
- **A build é indicada na barra de título,** então uma captura de tela mostra qual versão você está usando.
- **Novo ícone do Fritchii:** a pera, feita por um artista da comunidade.
- A janela Configurações se ajusta à sua tela, a página Controles segue o visual do Fritchii, e a tela cheia sem bordas funciona a partir de uma janela maximizada (zortos293/Lichii#1895).
- Em tela cheia, as capturas de tela são salvas direto na pasta (a janela Salvar como podia abrir atrás do jogo e parecer um congelamento).
- Se a atualização não encontrar o `lichii.exe` depois de descompactar, o Fritchii avisa que provavelmente foi o seu antivírus. O botão Doar abre o Buy Me a Coffee.

#### Para relatórios de bugs

- **Ajuda > Exportar log diz muito mais:** o que segurou cada quadro lento, um relatório completo quando o jogo congela por 5 s ou mais, detalhes da falha de GPU depois de um crash de GPU, cada pausa com a sua causa (perda de foco, suspensão, uma caixa de diálogo), um resumo da execução com os mínimos de 1% e 0,1%, e a build. Se algo der errado, exporte o log logo em seguida e poste no canal Issues com o modelo da sua GPU.
- **Ajuda > Modo de profiling:** ative, jogue no ponto com o problema, exporte o log e envie o zip. Uma barra vermelha sobre o jogo lembra que ele está ligado; desative depois.

#### Problemas conhecidos

<!-- Forma resumida de known-issues.md (a lista principal): mantenha as duas em sincronia. -->
- **Quedas curtas de desempenho ao matar inimigos:** cerca de metade da frequência e bem mais curtas que antes, mas ainda não eliminadas.
- **Primeira execução depois da grande preparação de shaders:** se o jogo der crash durante o primeiro carregamento de área, inicie-o de novo uma vez.
- **Intel Arc:** funciona, mas a preparação de shaders é lenta e coisas novas ainda podem congelar o jogo por um tempo. CPUs Intel com núcleos P e E: se o fps estiver em cerca de metade do esperado, adicione `EDEN_HYBRID_CORES=0` em Ferramentas > Parâmetros de inicialização.
- **Steam Deck:** cerca de 10-12 fps, abaixo do que o OoT precisa; uma Resolução menor (810p) ajuda.
- **Travamento raro de GPU,** cerca de uma vez a cada 200 inicializações aqui (em versões antigas também). Reinicie e envie o log se se repetir.
- **O DLSS treme para você?** Desmarque o Jitter do DLSS.

#### Créditos

Construído sobre o **Lichii de zortos293**: a base da qual o Fritchii evolui, e uma longa lista de pull requests dele foi portada nesta versão. Obrigado a zortos e a todos os contribuidores do Lichii cujo trabalho está aqui.


**Dark (Eglantine Dev)** pelo Dynamic FPS (integrado). **Blitchii, de DahBard,** pela correção do rastro de nado e das ondulações e por várias ideias (stacks de threads, a linha de log do modo de energia, trabalho nas travadinhas durante carregamentos). **CheezyEmu, de CheezerWheezer,** pelo FSR 4, pela correção dos blocos na névoa e na espuma e por um lote de correções para AMD. A equipe de recomp do OoT **Hookshot** pelas ideias por trás das verificações mais rápidas de dados da GPU. Correções para Intel Arc de um fork da comunidade, ideias para Linux e Steam Deck de um fork Linux da comunidade, ideias de memória de outros dois forks do Lichii e pull requests da comunidade no Fritchii (#36, #60, #64).

Todos do grupo de testes da v6 pelos logs, capturas de tela e longas sessões, e todo o Discord **Ocarina of Forks** pelo apoio moral, pelas ideias novas e pela ajuda na depuração.
