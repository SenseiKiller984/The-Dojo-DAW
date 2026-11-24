"""
The five Akai sets (XR-10, XE8, MPC3000, MPC2500, MPC5000): every sample's readable name and the
playable kits. make_kits.py adds selector kits for the samples no playable kit uses. The samples stay
the WAV files they came as (byte for byte).
"""

from roland import _pairs

XR10 = {f"Bassdrums/Bassdrum-{i:02d}": f"Kick {i:02d}" for i in range(1, 11)}
XR10.update({f"Snaredrums/Snaredrum-{i:02d}": f"Snare {i:02d}" for i in range(1, 11)})
XR10.update(_pairs(
    "Cymbals/Crash-01=Crash 1|Cymbals/Crash-02=Crash 2|Cymbals/Crash-03=Crash 3|Cymbals/Hat Closed=Hat Closed|"
    "Cymbals/Hat Middle=Hat Medium|Cymbals/Hat Open=Hat Open|Cymbals/Ride=Ride|Misc/Hit=Hit|Misc/Slap Bass=Slap Bass|"
    "Misc/Square Kick-01=Square Kick 1|Misc/Square Kick-02=Square Kick 2|Percussion/Agogo=Agogo|Percussion/Cabasa=Cabasa|"
    "Percussion/Clap=Clap|Percussion/Claves=Claves|Percussion/Conga-01=Conga 1|Percussion/Conga-02=Conga 2|"
    "Percussion/Conga-03=Conga 3|Percussion/Conga-04=Conga 4|Percussion/Cowbell=Cowbell|Percussion/Fingersanp=Finger Snap|"
    "Percussion/Guiro-01=Guiro 1|Percussion/Guirro-02=Guiro 2|Percussion/Rim Shot-01=Rim Shot 1|"
    "Percussion/Rim Shot-02=Rim Shot 2|Percussion/Tambourine=Tambourine|Percussion/Timbale H=Timbale High|"
    "Percussion/Timbale L=Timbale Low|Percussion/Toma H-01=Tom High 1|Percussion/Tom H-02=Tom High 2|"
    "Percussion/Tom L-01=Tom Low 1|Percussion/Tom L-02=Tom Low 2|Percussion/Tom M-01=Tom Mid 1|Percussion/Tom M-02=Tom Mid 2|"
    "Percussion/Triangle=Triangle|Percussion/Vibrator=Vibraslap|Percussion/Whistle=Whistle"))

# The XE8's internal sounds and its expansion card's ("Int", "Ext").
XE8 = _pairs(
    "Block 1 Ext_14=Block 1 Ext|Block 2 Ext_15=Block 2 Ext|Clap Int_15=Clap Int|Cowbell 1 Ext_12=Cowbell 1 Ext|"
    "Cowbell 2 Ext_13=Cowbell 2 Ext|Crack Int_16=Crack Int|Crash Ext_11=Crash Ext|Crash Int_13=Crash Int|"
    "Electrotom Int_4=Electro Tom Int|Hat Closed Ext_4=Hat Closed Ext|Hat Closed Int_5=Hat Closed Int|"
    "Hat Medium Int_6=Hat Medium Int|Hat Open Ext_5=Hat Open Ext|Hat Open Int_7=Hat Open Int|Kick 1 Int_1=Kick 1 Int|"
    "Kick 2 Int_2=Kick 2 Int|Kick 3 Int_3=Kick 3 Int|Kick Ext_1=Kick Ext|Orch Ext_16=Orchestra Hit Ext|Ride Ext_10=Ride Ext|"
    "Ride Int_14=Ride Int|Sidestick Ext_3=Side Stick Ext|Snare 1 Int_8=Snare 1 Int|Snare 2 Int_9=Snare 2 Int|"
    "Snare 3 Int_10=Snare 3 Int|Snare Ext_2=Snare Ext|Tom 1 Ext_6=Tom 1 Ext|Tom 2 Ext_7=Tom 2 Ext|Tom 3 Ext_8=Tom 3 Ext|"
    "Tom 4 Ext_9=Tom 4 Ext|Tom High Int_12=Tom High Int|Tom Low Int_11=Tom Low Int")

MPC3000 = _pairs(
    "51e_Shaker=Shaker|Agogo_Fx_Hi=Agogo FX High|Agogo_Fx_Low=Agogo FX Low|Ago_Go_35ed=Agogo 35|Ago_Go_36ed=Agogo 36|"
    "Bobby_Sn=Bobby Snare|Bongo_Hi_Op=Bongo High Open|Bongo_Hi_Slap=Bongo High Slap|Bongo_Lo_Op=Bongo Low Open|"
    "Bongo_Lo_Slap=Bongo Low Slap|Cabasa=Cabasa|Castanet=Castanets|Chekereshake=Shekere Shake|Chekere_Shot=Shekere Shot|"
    "Claves=Claves|Cl_Hat_3001=Hat Closed|Cm_Hat_Cl=CM Hat Closed|Cm_Hat_Op=CM Hat Open|Conga_Cl_Mt=Conga Closed Mute|"
    "Conga_Hi_Op=Conga High Open|Conga_L_Toe=Conga Low Toe|Conga_Op=Conga Open|Cowbell_3001=Cowbell|Cuica_Hi=Cuica High|"
    "Cuica_Lo=Cuica Low|Cymbal_3001=Cymbal 1|Cymbal_3002=Cymbal 2|Cymbal_3003=Cymbal 3|Dance_Kick=Dance Kick|"
    "Finger_Snaps=Finger Snaps|Fx_Cabasa=FX Cabasa|Fx_Cymbal=FX Cymbal|Fx_Hat_Op=FX Hat Open|Fx_Hh_Cl=FX Hat Closed|"
    "Fx_Mix_Tom=FX Mix Tom|Fx_Ride=FX Ride|Fx_Sn_2=FX Snare 2|Fx_Syn_Sn=FX Synth Snare|Gatesn_3002=Gated Snare|"
    "Gtd_Cabasa=Gated Cabasa|Gtd_Clap=Gated Clap|Gtd_Cowbell=Gated Cowbell|Gtd_H_Con_Rl=Gated Conga High Roll|"
    "Gtd_L_Con_Rl=Gated Conga Low Roll|Gtd_M_Con_Rl=Gated Conga Mid Roll|Gtd_Timb_Hi=Gated Timbale High|"
    "Gtd_Timb_Lo=Gated Timbale Low|Gtd_W_Block=Gated Wood Block|Guiro=Guiro|Guiro_Shot=Guiro Shot|Heavy_Kick=Heavy Kick|"
    "Huge_Tom=Huge Tom|Jv_Cymb_L=JV Cymbal|Log_Drum_Hi=Log Drum High|Log_Drum_Low=Log Drum Low|Maracas_3001=Maracas|"
    "Marktree_L_H=Mark Tree|Md_Hat_3001=Hat Medium|Metal_Flange=Metal Flange|Mg_Kick_10=MG Kick|Mh_Amb_Sn1=MH Ambient Snare|"
    "Mh_Heavy_Kik=MH Heavy Kick|Mix_Hat_Pedl=Mix Hat Pedal|Mix_Sn28gtd=Mix Snare Gated|Op_Hat_3001=Hat Open|"
    "Picolo_Sn_3001=Piccolo Snare|Res_Noise=Resonant Noise|Ride_3001=Ride 1|Ride_3002=Ride 2|Ride_3003=Ride 3|"
    "Rl_N_Tomf=Roll Tom Floor|Rl_N_Tomh=Roll Tom High|Rl_N_Toml=Roll Tom Low|Rl_N_Tomm=Roll Tom Mid|"
    "S2_Tamb1_Eq=Tambourine EQ|Side_Stick=Side Stick|Slay_Bell=Sleigh Bell|Snare_3002=Snare|Splash_3001=Splash 1|"
    "Splf_Kick_Mh=SPLF Kick|Std_Timb_H=Timbale High|Std_Timb_L=Timbale Low|Syn_Hh_Cl=Synth Hat Closed|"
    "Syn_Hh_Op=Synth Hat Open|Syn_Kick=Synth Kick|Syn_Mix_Tom=Synth Mix Tom|Talking_Down=Talking Drum Down|"
    "Talking_Hi=Talking Drum High|Talking_Low=Talking Drum Low|Talking_Up=Talking Drum Up|Tambourine_3001=Tambourine 1|"
    "Tamb_3001=Tambourine 2|Thumb_Noiz=Thumb Noise|Triangle_Op=Triangle Open|Whistle_High=Whistle High|"
    "Wood_Block=Wood Block|_Splash=Splash 2")

_AC = "Drum-Percussion/Acoustic kit/"
_E1 = "Drum-Percussion/Electric Kit/E_Kit1/"
_E2 = "Drum-Percussion/Electric Kit/E_Kit2/"
_PK = "Drum-Percussion/Percussion Kit/"
MPC2500 = _pairs(
    f"{_AC}Amb_Bd_1=Ambient Kick|{_AC}Amb_Ftm2=Ambient Tom Floor|{_AC}Amb_Hhcl=Ambient Hat Closed|"
    f"{_AC}Amb_Hhop=Ambient Hat Open|{_AC}Amb_Hhpd=Ambient Hat Pedal|{_AC}Amb_Htm=Ambient Tom High|"
    f"{_AC}Amb_Ltm2=Ambient Tom Low|{_AC}Amb_Mtm=Ambient Tom Mid|{_AC}Amb_Rim1=Ambient Rim|{_AC}Amb_Sn13=Ambient Snare 13|"
    f"{_AC}Amb_Sn_5=Ambient Snare 5|{_AC}China_1=China|{_AC}Crash_1=Crash 1|{_AC}Crash_5=Crash 5|{_AC}Crash_6=Crash 6|"
    f"{_AC}Ride_1=Ride 1|{_AC}Ride_3=Ride 3|{_AC}Splash_1=Splash|"
    "Drum-Percussion/Djambe/BAS=Djembe Bass|Drum-Percussion/Djambe/CL=Djembe Closed|"
    "Drum-Percussion/Djambe/CSL=Djembe Closed Slap|Drum-Percussion/Djambe/OP=Djembe Open|"
    "Drum-Percussion/Djambe/OSL=Djembe Open Slap|"
    f"{_E1}Clap=E-Kit 1 Clap|{_E1}Clave=E-Kit 1 Claves|{_E1}Cowbell=E-Kit 1 Cowbell|{_E1}Cymbal-Hi=E-Kit 1 Cymbal High|"
    f"{_E1}Cymbal_Lo=E-Kit 1 Cymbal Low|{_E1}Hat_Close=E-Kit 1 Hat Closed|{_E1}Hat_Long=E-Kit 1 Hat Open|"
    f"{_E1}Hat_Mediu=E-Kit 1 Hat Medium|{_E1}Hi Conga=E-Kit 1 Conga High|{_E1}Hi Tom=E-Kit 1 Tom High|"
    f"{_E1}Kick_Long=E-Kit 1 Kick Long|{_E1}Kick_Shor=E-Kit 1 Kick Short|{_E1}Lo Conga=E-Kit 1 Conga Low|"
    f"{_E1}Lo Tom=E-Kit 1 Tom Low|{_E1}Maracas=E-Kit 1 Maracas|{_E1}Md Conga=E-Kit 1 Conga Mid|{_E1}Md Tom=E-Kit 1 Tom Mid|"
    f"{_E1}Rimshot=E-Kit 1 Rim Shot|{_E1}Snare_Hi1=E-Kit 1 Snare High 1|{_E1}Snare_Hi2=E-Kit 1 Snare High 2|"
    f"{_E1}Snare_Hi3=E-Kit 1 Snare High 3|{_E1}Snare_Lo1=E-Kit 1 Snare Low 1|{_E1}Snare_Lo2=E-Kit 1 Snare Low 2|"
    f"{_E1}Snare_Lo3=E-Kit 1 Snare Low 3|"
    f"{_E2}Clap=E-Kit 2 Clap|{_E2}Crash=E-Kit 2 Crash|{_E2}Hat_Close=E-Kit 2 Hat Closed|{_E2}Hat_Med=E-Kit 2 Hat Medium|"
    f"{_E2}Hat_Open=E-Kit 2 Hat Open|{_E2}Kick_Long=E-Kit 2 Kick|{_E2}Ride=E-Kit 2 Ride|{_E2}Snare1_1=E-Kit 2 Snare 1A|"
    f"{_E2}Snare1_2=E-Kit 2 Snare 1B|{_E2}Snare1_3=E-Kit 2 Snare 1C|{_E2}Snare2_1=E-Kit 2 Snare 2A|"
    f"{_E2}Snare2_2=E-Kit 2 Snare 2B|{_E2}Snare2_3=E-Kit 2 Snare 2C|{_E2}Tom_Hi=E-Kit 2 Tom High|{_E2}Tom_Lo=E-Kit 2 Tom Low|"
    f"{_E2}Tom_Mid=E-Kit 2 Tom Mid|"
    f"{_PK}Agogo Hi=Agogo High|{_PK}Agogo Lo=Agogo Low|{_PK}Belltree=Bell Tree|{_PK}Block Hi=Block High|"
    f"{_PK}Block Lo=Block Low|{_PK}Bongo Hi=Bongo High|{_PK}Bongo Lo=Bongo Low|{_PK}Cabasa 1=Cabasa 1|{_PK}Cabasa 2=Cabasa 2|"
    f"{_PK}Castanet=Castanets|{_PK}Clave=Claves|{_PK}Conga Hi=Conga High|{_PK}Conga Lo 1=Conga Low 1|"
    f"{_PK}Conga Lo 2=Conga Low 2|{_PK}Cowbell=Cowbell|{_PK}Guica High=Cuica High|{_PK}Guica Low=Cuica Low|"
    f"{_PK}Guiro L=Guiro Long|{_PK}Guiro S=Guiro Short|{_PK}Sleigh=Sleigh Bell|{_PK}Tambourine=Tambourine|"
    f"{_PK}Triangle Closed=Triangle Closed|{_PK}Triangle Open=Triangle Open|{_PK}Vibraslap=Vibraslap|"
    f"{_PK}Whistle Long=Whistle Long|{_PK}Whistle Short=Whistle Short|"
    "Drum-Percussion/Wadaiko/Wadaiko1=Wadaiko 1|Drum-Percussion/Wadaiko/Wadaiko2=Wadaiko 2|"
    "Other/NOISE/Amp Ham Noise=Amp Hum Noise|Other/NOISE/Record Noise1=Record Noise 1|Other/NOISE/Record Noise2=Record Noise 2|"
    "INTERNAL/A-Sand=A-Sand|INTERNAL/Ah=Vox Ah|INTERNAL/Akai=Vox Akai|INTERNAL/Arp_Wave=Arp Wave|INTERNAL/B-Kick=B Kick|"
    "INTERNAL/B-Scratch=B Scratch|INTERNAL/Basspp=Bass|INTERNAL/Blip=Blip|INTERNAL/Clock=Clock|INTERNAL/Cymbal1=Cymbal|"
    "INTERNAL/Drill=Drill|INTERNAL/E-Cloud=E-Cloud|INTERNAL/E-Kick=E Kick|INTERNAL/E-Snare=E Snare|INTERNAL/Elev_A=Elevator A|"
    "INTERNAL/Elev_B=Elevator B|INTERNAL/Elev_C=Elevator C|INTERNAL/Gt_Mute=Guitar Mute|INTERNAL/Gt_Stroke=Guitar Stroke|"
    "INTERNAL/Guitar75mpp2=Guitar Loop 75|INTERNAL/HH1-Cl=Hat 1 Closed|INTERNAL/HH1-Op=Hat 1 Open|INTERNAL/HH2-Cl=Hat 2 Closed|"
    "INTERNAL/HH2-Op=Hat 2 Open|INTERNAL/Herge=Herge|INTERNAL/Hiphop75mpp=Hip Hop Loop 75|INTERNAL/House75mpp=House Loop 75|"
    "INTERNAL/Jdkick=JD Kick|INTERNAL/JdopHH=JD Hat Open|INTERNAL/Jdsnare1=JD Snare 1|INTERNAL/Jdsnare2=JD Snare 2|"
    "INTERNAL/K H Cl=K Hat Closed|INTERNAL/K Med Hat=K Hat Medium|INTERNAL/Keen K1=Keen Kick 1|INTERNAL/Keen K2=Keen Kick 2|"
    "INTERNAL/Keen S1=Keen Snare 1|INTERNAL/Keen S2=Keen Snare 2|INTERNAL/Kik=Kick|INTERNAL/L-Cymbal=L Cymbal|"
    "INTERNAL/L-Kick=L Kick|INTERNAL/L-Snare=L Snare|INTERNAL/Lo Bass=Low Bass|INTERNAL/Loop1xpp=Loop 1|INTERNAL/Loop2xpp=Loop 2|"
    "INTERNAL/M-Hat=M Hat|INTERNAL/M-Melt=M Melt|INTERNAL/M-Shake=M Shaker|INTERNAL/M-Snare=M Snare|INTERNAL/M-Sprink=M Sprinkle|"
    "INTERNAL/Metal=Metal|INTERNAL/Mpc2500=Vox MPC2500|INTERNAL/Mute Sam=Mute|INTERNAL/Noise=Noise|INTERNAL/Noiz_Lp1=Noise Loop|"
    "INTERNAL/Noize=Noise Short|INTERNAL/Orch_1=Orchestra Hit|INTERNAL/Pad1=Pad|INTERNAL/Piatti=Piatti|"
    "INTERNAL/R&B75mpp=R&B Loop 75|INTERNAL/Scratch75lpp=Scratch Loop 75|INTERNAL/Sine=Sine|INTERNAL/Snare=Snare|"
    "INTERNAL/Square=Square|INTERNAL/Swepuls=Sweep Pulse|INTERNAL/Switch=Switch|INTERNAL/Syn-Bass=Synth Bass|"
    "INTERNAL/Synbass1=Synth Bass 1|INTERNAL/Tabra75lpp=Tabla Loop 75|INTERNAL/Tambalin=Tambourine Short|"
    "INTERNAL/Techno75mpp=Techno Loop 75|INTERNAL/Tri=Triangle|INTERNAL/Tron=Tron|INTERNAL/Under_Water=Under Water|"
    "INTERNAL/V-Scratch=V Scratch|INTERNAL/Voice1=Vox Voice 1|INTERNAL/Voice2=Vox Voice 2|INTERNAL/W-N=White Noise")
# The tabla strokes: the bayan (bass drum) and the tabla (treble drum).
for _i in list(range(1, 9)) + [10]:
    MPC2500[f"INTERNAL/Byn{_i}"] = f"Tabla Bayan {_i:02d}"
for _i in range(1, 33):
    MPC2500[f"INTERNAL/T{_i:02d}"] = f"Tabla {_i:02d}"

_R1 = "Internal Rom 1/"
_R2 = "Internal Rom 2/"
MPC5000 = _pairs(
    f"{_R1}1 (334)=Hit 334|{_R1}5000=Vox 5000|{_R1}Alien=Alien|{_R1}Bbdrum1=Big Bass Drum Kick 1|"
    f"{_R1}Bbdrum2=Big Bass Drum Kick 2|{_R1}Bd1=Kick 1|{_R1}Bdrum=Kick 2|{_R1}Beow=Beow|{_R1}Bigtom=Big Tom|"
    f"{_R1}Buzergli=Buzergli|{_R1}Cabasa=Cabasa|{_R1}Clap=Clap|{_R1}Clap4som=Clap 4 Some|{_R1}Claves=Claves|"
    f"{_R1}Clpvintg=Clap Vintage|{_R1}Cngafrol=Conga Roll|{_R1}Conga=Conga|{_R1}Cowbell1=Cowbell 1|{_R1}Cowbell2=Cowbell 2|"
    f"{_R1}Crashcym=Crash|{_R1}Crosstix=Cross Stick|{_R1}Cym1=Cymbal 1|{_R1}Cym2=Cymbal 2|{_R1}Cym3=Cymbal 3|"
    f"{_R1}Cymmalet=Cymbal Mallet|{_R1}Cymride=Ride|{_R1}Cymshort=Cymbal Short|{_R1}Deeptom=Deep Tom|{_R1}Drumbo=Drumbo|"
    f"{_R1}Esnare=E Snare|{_R1}Finger=Finger Snap|{_R1}Fish=Fish|{_R1}Fishs=Fish Short|{_R1}Floortom=Floor Tom|"
    f"{_R1}Fxdrill=FX Drill|{_R1}Fxelect=FX Electric|{_R1}Fxnoi2=FX Noise|{_R1}Fxrefle=FX Reflex|{_R1}Fxtrash=FX Trash|"
    f"{_R1}Gorgon=Gorgon|{_R1}Hh1980op=Hat 1980 Open|{_R1}Hhclosed=Hat Closed|{_R1}Hhgamemd=Hat Game Medium|"
    f"{_R1}Hhhalocl=Hat Halo Closed|{_R1}Hhhaloop=Hat Halo Open|{_R1}Hhopen=Hat Open|{_R1}Hhpedal=Hat Pedal|"
    f"{_R1}Hhtamb=Hat Tambourine|{_R1}Hicrash=High Crash|{_R1}Hitgsyn=Synth Hit G|{_R1}Hitsyns3=Synth Hit 3|{_R1}K2=Kick 3|"
    f"{_R1}Kik8oh4=Kick 804|{_R1}Kikboom=Kick Boom|{_R1}Kikdeep=Kick Deep|{_R1}Kikthick=Kick Thick|"
    f"{_R1}Linn 9000 Bell=Linn 9000 Bell|{_R1}Linn 9000 Cabasa=Linn 9000 Cabasa|{_R1}Linn 9000 Clap=Linn 9000 Clap|"
    f"{_R1}Linn 9000 Conga=Linn 9000 Conga|{_R1}Linn 9000 Cowb1=Linn 9000 Cowbell|{_R1}Linn 9000 Crash=Linn 9000 Crash|"
    f"{_R1}Linn 9000 Hat 1=Linn 9000 Hat Open|{_R1}Linn 9000 Hat Cl=Linn 9000 Hat Closed|{_R1}Linn 9000 Kick=Linn 9000 Kick|"
    f"{_R1}Linn 9000 Ride=Linn 9000 Ride|{_R1}Linn 9000 Snare=Linn 9000 Snare|{_R1}Linn 9000 Stick=Linn 9000 Side Stick|"
    f"{_R1}Linn 9000 Tamb=Linn 9000 Tambourine|{_R1}Linn 9000 Tom=Linn 9000 Tom|{_R1}Loop 1a=Loop 1A|{_R1}Loop 1b=Loop 1B|"
    f"{_R1}Loop 5a=Loop 5A|{_R1}Loop 5b=Loop 5B|{_R1}Loop 6a=Loop 6A|{_R1}Maracsht=Maracas Shot|{_R1}Martree1=Mark Tree|"
    f"{_R1}Milbdrum=Military Drum|{_R1}Mpc1 Vox=Vox MPC 1|{_R1}Mpc2 Vox=Vox MPC 2|{_R1}Mpc3 Vox=Vox MPC 3|"
    f"{_R1}On The Cell D 1=Vox On The Cell|"
    f"{_R2}Opclhat=Hat Open-Closed|{_R2}Pang=Pang|{_R2}Rim=Rim|{_R2}Scrvox=Scratch Vox|{_R2}Shknyz=Shaker Noise|"
    f"{_R2}Snare=Snare|{_R2}Snare1=Snare 1|{_R2}Snrateo3=Snare Ateo|{_R2}Snrelect=Snare Electric|"
    f"{_R2}Solilla 808 Long=Solilla 808 Bass Long|{_R2}Solilla 808=Solilla 808 Bass|{_R2}Solilla Bass 1=Solilla Bass 1|"
    f"{_R2}Solilla Bass 2=Solilla Bass 2|{_R2}Solilla Clap=Solilla Clap|{_R2}Solilla Kick 1=Solilla Kick 1|"
    f"{_R2}Solilla Kick 2=Solilla Kick 2|{_R2}Solilla Piano=Solilla Piano|{_R2}Solilla Snare=Solilla Snare|"
    f"{_R2}Solilla Square=Solilla Square|{_R2}Solilla Vox 1=Solilla Vox 1|{_R2}Solilla Vox 2=Solilla Vox 2|"
    f"{_R2}Solilla Wobbler=Solilla Wobbler|{_R2}Ssnre=S Snare|{_R2}Ssnre1=S Snare 1|{_R2}Stonerepeater=Stone Repeater|"
    f"{_R2}Stonestr01=Stone Strings 1|{_R2}Stonestr02=Stone Strings 2|{_R2}Stonestr03=Stone Strings 3|"
    f"{_R2}Stonestr04=Stone Strings 4|{_R2}Symkick=Sym Kick|{_R2}Symsnre1=Sym Snare|{_R2}Symtom=Sym Tom Mid|"
    f"{_R2}Symtomhi=Sym Tom High|{_R2}Symtomlo=Sym Tom Low|{_R2}Synpunch=Synth Punch|{_R2}Tabla=Tabla|"
    f"{_R2}Tambour=Tambourine|{_R2}Timb=Timbale 1|{_R2}Timbale=Timbale 2|{_R2}Tom2=Tom|{_R2}Tymp=Timpani|"
    f"{_R2}Voxahsss=Vox Ahsss|{_R2}Voxlaugh=Vox Laugh|{_R2}Voxoh=Vox Oh")
for _i in range(1, 7):
    MPC5000[f"{_R2}Solilla Dirty {_i}"] = f"Solilla Dirty {_i}"
for _i in range(1, 8):
    MPC5000[f"{_R2}Solilla Hat {_i}"] = f"Solilla Hat {_i}"
for _i in range(1, 4):
    MPC5000[f"{_R2}Solilla Hihat {_i}"] = f"Solilla Hat {_i + 7}"
for _i in range(1, 17):
    MPC5000[f"{_R2}Solilla Stabs {_i:02d}"] = f"Solilla Stab {_i:02d}"

MACHINES = {
    "Akai XR-10": ("Akai XR-10", XR10),
    "Akai XE8": ("Akai XE8", XE8),
    "Akai MPC3000": ("Akai MPC3000", MPC3000),
    "Akai MPC2500": ("Akai MPC2500", MPC2500),
    "Akai MPC5000": ("Akai MPC5000", MPC5000),
}

# Playable kits (the layout of make_kits.py): name, machine, tempo, swing, groove, 16 pads, description.
KITS = [
    ("XR-10 Kit", "Akai XR-10", 112, 0.0, "pop",
     ["Kick 01", "Rim Shot 1", "Snare 01", "Clap", "Snare 02", "Tom Low 1", "Hat Closed", "Tom Mid 1",
      "Hat Medium", "Tom High 1", "Hat Open", "Conga 1", "Conga 2", "Crash 1", "Cowbell", "Ride"],
     "The XR-10 drum machine's kit: punchy, bright nineties drums."),
    ("XR-10 Kit 2", "Akai XR-10", 96, 0.12, "funk",
     ["Kick 02", "Rim Shot 2", "Snare 03", "Finger Snap", "Snare 04", "Tom Low 2", "Hat Closed", "Tom Mid 2",
      "Tambourine", "Tom High 2", "Hat Open", "Conga 3", "Conga 4", "Crash 2", "Agogo", "Cabasa"],
     "The XR-10's second kick and snares, with tambourine, agogo and cabasa."),
    ("XR-10 Latin Kit", "Akai XR-10", 104, 0.0, "latin",
     ["Kick 03", "Claves", "Snare 05", "Clap", "Cabasa", "Timbale Low", "Guiro 1", "Conga 1", "Tambourine",
      "Conga 2", "Guiro 2", "Conga 3", "Conga 4", "Triangle", "Cowbell", "Timbale High"],
     "The XR-10's latin percussion: congas, timbales, guiros, claves and more."),
    ("XE8 Kit", "Akai XE8", 116, 0.0, "pop",
     ["Kick 1 Int", "Crack Int", "Snare 1 Int", "Clap Int", "Snare 2 Int", "Tom Low Int", "Hat Closed Int", "Electro Tom Int",
      "Hat Medium Int", "Tom High Int", "Hat Open Int", "Kick 2 Int", "Kick 3 Int", "Crash Int", "Snare 3 Int", "Ride Int"],
     "The XE8 drum module's own sounds."),
    ("XE8 Ext Kit", "Akai XE8", 108, 0.0, "rock",
     ["Kick Ext", "Side Stick Ext", "Snare Ext", "Orchestra Hit Ext", "Block 1 Ext", "Tom 4 Ext", "Hat Closed Ext",
      "Tom 3 Ext", "Block 2 Ext", "Tom 2 Ext", "Hat Open Ext", "Tom 1 Ext", "Cowbell 2 Ext", "Crash Ext", "Cowbell 1 Ext", "Ride Ext"],
     "The XE8's expansion sounds: kick, snare, four toms, blocks, cowbells and an orchestra hit."),
    ("MPC3000 Kit", "Akai MPC3000", 92, 0.16, "boombap",
     ["Heavy Kick", "Side Stick", "Snare", "Gated Clap", "Piccolo Snare", "Roll Tom Low", "Hat Closed", "Roll Tom Mid",
      "Hat Medium", "Roll Tom High", "Hat Open", "Conga Open", "Conga High Open", "Cymbal 1", "Cowbell", "Ride 1"],
     "The MPC3000's own kit: heavy kick, snare and hats for boom-bap."),
    ("MPC3000 Kit 2", "Akai MPC3000", 98, 0.12, "boombap",
     ["MH Heavy Kick", "Finger Snaps", "MH Ambient Snare", "Gated Clap", "Bobby Snare", "Roll Tom Floor", "CM Hat Closed",
      "Huge Tom", "Mix Hat Pedal", "Synth Mix Tom", "CM Hat Open", "Dance Kick", "Gated Snare", "Cymbal 2", "Tambourine 1", "Ride 2"],
     "More MPC3000 drums: ambient snare, gated snare, big toms."),
    ("MPC3000 Latin Kit", "Akai MPC3000", 104, 0.0, "latin",
     ["MG Kick", "Claves", "Mix Snare Gated", "Gated Clap", "Cabasa", "Timbale Low", "Maracas", "Bongo Low Open",
      "Shaker", "Bongo High Open", "Guiro", "Conga Closed Mute", "Conga High Open", "Agogo 35", "Gated Cowbell", "Timbale High"],
     "The MPC3000's latin percussion."),
    ("MPC2500 Acoustic Kit", "Akai MPC2500", 100, 0.0, "rock",
     ["Ambient Kick", "Ambient Rim", "Ambient Snare 13", "Splash", "Ambient Snare 5", "Ambient Tom Floor", "Ambient Hat Closed",
      "Ambient Tom Low", "Ambient Hat Pedal", "Ambient Tom High", "Ambient Hat Open", "Ambient Tom Mid", "China", "Crash 1",
      "Crash 5", "Ride 1"],
     "The MPC2500's acoustic kit, recorded with the room."),
    ("MPC2500 Electric Kit", "Akai MPC2500", 120, 0.0, "electro",
     ["E-Kit 1 Kick Long", "E-Kit 1 Rim Shot", "E-Kit 1 Snare High 1", "E-Kit 1 Clap", "E-Kit 1 Snare Low 1", "E-Kit 1 Tom Low",
      "E-Kit 1 Hat Closed", "E-Kit 1 Tom Mid", "E-Kit 1 Hat Medium", "E-Kit 1 Tom High", "E-Kit 1 Hat Open", "E-Kit 1 Conga Low",
      "E-Kit 1 Conga High", "E-Kit 1 Cymbal High", "E-Kit 1 Cowbell", "E-Kit 1 Cymbal Low"],
     "The MPC2500's first electronic kit."),
    ("MPC2500 Electric Kit 2", "Akai MPC2500", 124, 0.0, "electro",
     ["E-Kit 2 Kick", "E-Kit 1 Claves", "E-Kit 2 Snare 1A", "E-Kit 2 Clap", "E-Kit 2 Snare 2A", "E-Kit 2 Tom Low",
      "E-Kit 2 Hat Closed", "E-Kit 2 Tom Mid", "E-Kit 2 Hat Medium", "E-Kit 2 Tom High", "E-Kit 2 Hat Open", "E-Kit 1 Kick Short",
      "E-Kit 1 Maracas", "E-Kit 2 Crash", "E-Kit 1 Conga Mid", "E-Kit 2 Ride"],
     "The MPC2500's second electronic kit."),
    ("MPC2500 Kit", "Akai MPC2500", 94, 0.16, "boombap",
     ["Kick", "Switch", "Snare", "M Snare", "Keen Snare 1", "B Kick", "Hat 1 Closed", "E Kick", "K Hat Closed", "Keen Kick 1",
      "Hat 1 Open", "JD Snare 1", "L Snare", "Cymbal", "M Shaker", "Hat 2 Open"],
     "The MPC2500's internal drums for hip hop."),
    ("MPC2500 Percussion Kit", "Akai MPC2500", 104, 0.0, "latin",
     ["Djembe Bass", "Claves", "Djembe Open Slap", "Castanets", "Cabasa 1", "Conga Low 1", "Guiro Short", "Bongo Low",
      "Tambourine", "Bongo High", "Guiro Long", "Conga Low 2", "Conga High", "Agogo High", "Cowbell", "Agogo Low"],
     "The MPC2500's percussion: djembe, congas, bongos, agogos and more."),
    ("MPC2500 Tabla Kit", "Akai MPC2500", 96, 0.0, "percussion",
     ["Tabla Bayan 01", "Tabla 01", "Tabla 02", "Tabla 03", "Tabla 04", "Tabla Bayan 02", "Tabla 05", "Tabla Bayan 03",
      "Tabla 06", "Tabla Bayan 04", "Tabla 07", "Tabla 08", "Tabla 09", "Tabla Bayan 05", "Tabla 10", "Tabla Bayan 06"],
     "Tabla strokes from the MPC2500: the bayan's low strokes and the tabla's."),
    ("MPC5000 Kit", "Akai MPC5000", 92, 0.16, "boombap",
     ["Kick Thick", "Cross Stick", "Snare", "Clap", "Snare 1", "Floor Tom", "Hat Closed", "Deep Tom", "Hat Pedal", "Tom",
      "Hat Open", "Conga", "Kick Deep", "Crash", "Cowbell 1", "Ride"],
     "The MPC5000's kit: thick kick, snare and hats."),
    ("MPC5000 Linn 9000 Kit", "Akai MPC5000", 112, 0.0, "pop",
     ["Linn 9000 Kick", "Linn 9000 Side Stick", "Linn 9000 Snare", "Linn 9000 Clap", "Linn 9000 Tambourine", "Linn 9000 Tom",
      "Linn 9000 Hat Closed", "Big Tom", "Linn 9000 Cabasa", "Sym Tom High", "Linn 9000 Hat Open", "Linn 9000 Conga",
      "Linn 9000 Bell", "Linn 9000 Crash", "Linn 9000 Cowbell", "Linn 9000 Ride"],
     "The Linn 9000 sounds in the MPC5000's library."),
    ("MPC5000 Solilla Kit", "Akai MPC5000", 140, 0.0, "trap",
     ["Solilla Kick 1", "Solilla 808 Bass", "Solilla Snare", "Solilla Clap", "Solilla Kick 2", "Solilla 808 Bass Long",
      "Solilla Hat 1", "Solilla Dirty 1", "Solilla Hat 2", "Solilla Dirty 2", "Solilla Hat 3", "Solilla Vox 1", "Solilla Vox 2",
      "Solilla Stab 01", "Solilla Wobbler", "Solilla Hat 8"],
     "The MPC5000's Solilla sounds: kicks, 808 bass, snare, clap, hats and stabs."),
    ("MPC5000 Sym Kit", "Akai MPC5000", 100, 0.0, "funk",
     ["Sym Kick", "Rim", "Sym Snare", "Clap Vintage", "E Snare", "Sym Tom Low", "Hat Halo Closed", "Sym Tom Mid",
      "Hat Game Medium", "Sym Tom High", "Hat Halo Open", "Kick Boom", "Synth Punch", "Cymbal 1", "Hat Tambourine", "Cymbal Short"],
     "The MPC5000's Sym kit and more of its drums."),
]
