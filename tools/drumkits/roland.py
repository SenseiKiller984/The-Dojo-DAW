"""
The five Roland sets (MC-202, MC-303, MC-808, R-8 MKII, R-70): every sample's readable name and the
playable kits. make_kits.py adds selector kits for the samples no playable kit uses, so that every
sample is on a pad. The samples stay the WAV files they came as (byte for byte).
"""


def _pairs(text):
    """ "orig=Name|orig=Name" -> dict (originals relative to the machine's folder, no extension)."""
    out = {}
    for item in text.split("|"):
        item = item.strip()
        if item:
            original, name = item.split("=")
            out[original.strip()] = name.strip()
    return out


MC202 = _pairs("BM1=Bass 1|BM2=Bass 2|BM3=Bass 3|BM4=Bass 4|BM5=Bass 5|SHT1=Hit 1|SHT2=Hit 2|SHT3=Hit 3|SHT4=Hit 4")

# The MC-808 samples came unnamed (Sample01-59): named by what they sound like (measured: length,
# decay, brightness, pitch, low end), their number kept.
MC808 = _pairs(
    "Sample01=Kick (01)|Sample02=Kick (02)|Sample03=Kick Long (03)|Sample04=Snare (04)|Sample05=Snare (05)|"
    "Sample06=Clap (06)|Sample07=Rim (07)|Sample08=Snare Short (08)|Sample09=Hat Closed (09)|Sample10=Perc Metal (10)|"
    "Sample11=Hat Closed (11)|Sample12=Hat Closed (12)|Sample13=Hat Closed (13)|Sample14=Hat Open (14)|"
    "Sample15=Hat Open (15)|Sample16=Cymbal (16)|Sample17=Sub Kick (17)|Sample18=Sub Kick (18)|"
    "Sample19=Sub Kick Long (19)|Sample20=Perc Tone (20)|Sample21=Hat Open (21)|Sample22=Crash (22)|"
    "Sample23=Ride (23)|Sample31=Sub Bass (31)|Sample32=Sub Bass (32)|Sample33=Sub Bass (33)|Sample34=Sub Bass (34)|"
    "Sample35=808 Kick (35)|Sample36=808 Kick (36)|Sample58=Tom (58)|Sample59=Tom (59)")

MC303 = _pairs(
    "606bd1=606 Kick 1|606bd2=606 Kick 2|606bd3=606 Kick 3|606ch=606 Hat Closed|606sd1=606 Snare 1|606sd2=606 Snare 2|"
    "606sd3=606 Snare 3|626shake=626 Shaker|707ch=707 Hat Closed|707clap=707 Clap|707oh=707 Hat Open|78ch=78 Hat Closed|"
    "78cowbel=78 Cowbell|78guiro=78 Guiro|78hitom=78 Tom High|78lotom=78 Tom Low|78metalb=78 Metal Beat|78midtom=78 Tom Mid|"
    "78oh=78 Hat Open|78sd=78 Snare|78tamb=78 Tambourine|80809sd=808-909 Snare|Achitom1=Acoustic Tom High 1|"
    "Achitom2=Acoustic Tom High 2|Aclotom1=Acoustic Tom Low 1|Aclotom2=Acoustic Tom Low 2|Acmidtm2=Acoustic Tom Mid 2|"
    "Acmidtom=Acoustic Tom Mid 1|Afrofeet=Afro Feet|Asiangon=Asian Gong|Bamboosd=Bamboo Snare|Bamboost=Bamboo Stick|"
    "Blipbd=Blip Kick|Brushslp=Brush Slap|Brushswi=Brush Swish|Brushtap=Brush Tap|Cabasado=Cabasa Down|Cabasaup=Cabasa Up|"
    "Cavebd=Cave Kick 1|Cavebd2=Cave Kick 2|Claves=Claves|Clipsd1=Clip Snare 1|Clipsd2=Clip Snare 2|Cowbell=Cowbell|"
    "Distbd1=Distorted Kick 1|Distbd2=Distorted Kick 2|Distbd3=Distorted Kick 3|Drybd1=Dry Kick 1|Drybd2=Dry Kick 2|"
    "Drybd3=Dry Kick 3|Drysd1=Dry Snare 1|Drysd2=Dry Snare 2|Elecbd=Electric Kick|Elechibo=Electric Bongo High|"
    "Eleclobo=Electric Bongo Low|Elecloto=Electric Tom Low 1|Elecsd1=Electric Snare|Fingersn=Finger Snap|"
    "Fnkygost=Funky Ghost Snare|Funkysd1=Funky Snare 1|Funkysd2=Funky Snare 2|Fxsd1=FX Snare 1|Fxsd2=FX Snare 2|"
    "Hardclap=Hard Clap|Hardsd1=Hard Snare|Hc2clap=HC2 Clap|Hiagogo=Agogo High|Hibamboo=Bamboo High|Hibongoo=Bongo High Open|"
    "Hicongao=Conga High Open|Hicongas=Conga High Slap|Hihyoshi=Hyoshigi High|Hipclap1=Hip Clap|Hisyntht=Synth Tom High|"
    "Hitimbal=Timbale High|Hittamb=Hit Tambourine|Hiwoodbl=Wood Block High|Hrtbeat=Heartbeat|Hypersd1=Hyper Snare 1|"
    "Hypersd2=Hyper Snare 2|Jnglebd2=Jungle Kick 2|Jnglesd1=Jungle Snare 1|Jnglesd2=Jungle Snare 2|Jnglesd3=Jungle Snare 3|"
    "Jnglesd4=Jungle Snare 4|Jngletam=Jungle Tambourine|Junglebd=Jungle Kick 1|Lechito1=Electric Tom High 1|"
    "Lechito2=Electric Tom High 2|Lecloto2=Electric Tom Low 2|Lecmidt1=Electric Tom Mid 1|Lecmidt2=Electric Tom Mid 2|"
    "Loagogo=Agogo Low|Lobamboo=Bamboo Low|Lobongoo=Bongo Low Open|Locongao=Conga Low Open|Lohyoshi=Hyoshigi Low|"
    "Longguir=Guiro Long|Longwhis=Whistle Long|Losyntht=Synth Tom Low|Lotimbal=Timbale Low|Lowoodbl=Wood Block Low|"
    "Maracas=Maracas|Mgblip1=MG Blip|Midbambo=Bamboo Mid|Midsynth=Synth Tom Mid|Mutecuic=Cuica Mute|Mutepand=Pandeiro Mute|"
    "Mutesurd=Surdo Mute|Mutetria=Triangle Mute|Opencuic=Cuica Open|Openpand=Pandeiro Open|Opensurd=Surdo Open|"
    "Opentria=Triangle Open|Rapclap1=Rap Clap 1|Rapclap2=Rap Clap 2|Rapsd=Rap Snare|Realch1=Real Hat Closed 1|"
    "Realch2=Real Hat Closed 2|Realclap=Real Clap|Realoh1=Real Hat Open 1|Realoh2=Real Hat Open 2|Realph1=Real Hat Pedal 1|"
    "Realph2=Real Hat Pedal 2|Ridecym2=Ride 2|Ridecymb=Ride 1|Rimsd1=Rim Snare 1|Rimsd2=Rim Snare 2|Rimshot=Rim Shot|"
    "Roomch=Room Hat Closed|Roomoh=Room Hat Open|Shakecla=Shaker Clap|Shaker=Shaker|Shaketam=Shaker Tambourine|"
    "Shortgui=Guiro Short|Shortwhi=Whistle Short|Slap=Slap|Tablabay=Tabla Bayan|Tambouri=Tambourine|Tambsd1=Tambourine Snare|"
    "Tightsd=Tight Snare|Tinyrim2=Tiny Rim 2|Tinyrim3=Tiny Rim 3|Tinyrim4=Tiny Rim 4|Tinysd=Tiny Snare|Udo=Udo|"
    "Vibrasla=Vibraslap|Whitnoiz=White Noise")

R8 = _pairs(
    "55Clave=55 Claves|78Guiro=78 Guiro|78Mbeat=78 Metal Beat|78_Bng=78 Bongo|78_Chh=78 Hat Closed|78_Cow=78 Cowbell|"
    "78_K=78 Kick|78_Marc=78 Maracas|78_Ohh=78 Hat Open|78_S=78 Snare|78_Tamb=78 Tambourine|808Clap=808 Clap|"
    "808Clav=808 Claves|808Cng=808 Conga|808Marc=808 Maracas|808Side=808 Side Stick|808_Chh=808 Hat Closed|808_K=808 Kick|"
    "808_Ohh=808 Hat Open|808_S2=808 Snare 2|808_T=808 Tom|909Side=909 Side Stick|909_Chh=909 Hat Closed|909_K=909 Kick|"
    "909_Ohh=909 Hat Open|909_S=909 Snare|909_T=909 Tom|Agogo1=Agogo|Ambo_K=Ambient Kick|Atak_T2=Attack Tom 2|"
    "Atak_T3=Attack Tom 3|Atak_T4=Attack Tom 4|Back_C2=Back Cymbal 2|Back_Go=Back Gong|Back_S1=Back Snare 1|"
    "Back_T1=Back Tom 1|Bell_C3=Bell Cymbal 3|Beltree=Bell Tree|Boing_K=Boing Kick|Boing_S=Boing Snare|Bottm_K=Bottom Kick|"
    "Brcl_H1=Brush Hat Closed|Brite_S=Bright Snare|Brop_H1=Brush Hat Open|Brrd_C1=Brush Ride|Cabasa1=Cabasa|"
    "Castane=Castanets|China_C=China|Chok_C1=Choke Cymbal|Clave1=Claves|Clsd_H2=Hat Closed 2|Clsd_H4=Hat Closed 4|"
    "Clsd_H5=Hat Closed 5|Clsd_H6=Hat Closed 6|Clsd_H7=Hat Closed 7|Combo_S=Combo Snare|Con_Bd2=Concert Bass Drum|"
    "Cowbel1=Cowbell 1|Cowbel2=Cowbell 2|Crsh_C10=Crash 10|Crsh_C9=Crash 9|Dance_S=Dance Snare|Dblh_K3=Double Head Kick 3|"
    "Dblh_K4=Double Head Kick 4|Deep_K1=Deep Kick|Doom_T1=Doom Tom 1|Doom_T2=Doom Tom 2|Doom_T3=Doom Tom 3|Dryclap=Dry Clap|"
    "Dry_K1=Dry Kick|Echo_S=Echo Snare|Ez_K=EZ Kick|Face_K=Face Kick|Fat_S1=Fat Snare|Floor_K=Floor Kick|Flop_K=Flop Kick|"
    "Fngsnp2=Finger Snap|Funk_S=Funk Snare|Gate_K=Gated Kick 1|Gate_K2=Gated Kick 2|Gate_S=Gated Snare|Gong=Gong|"
    "Gunsht1=Gun Shot|Hand_C1=Hand Clap|Hard_K=Hard Kick|Heavy_K=Heavy Kick|High_Bg=Bongo High|High_Q=High Q|"
    "High_Tb=Timbale High|Holo_T1=Hollow Tom 1|Holo_T2=Hollow Tom 2|Holo_T3=Hollow Tom 3|Holo_T4=Hollow Tom 4|"
    "House_K=House Kick|House_S=House Snare|Kalimba=Kalimba|LA_S=LA Snare|Light_S=Light Snare|Lng_Gui=Guiro Long|"
    "Logdrum=Log Drum|Low_Bg=Bongo Low|Low_Cg=Conga Low|Low_Tb=Timbale Low|Maracas=Maracas|Mondo_K=Mondo Kick|"
    "Muscl_K=Muscle Kick|Mut_Cui=Cuica Mute|Mut_Pnd=Pandeiro Mute|Mut_Srd=Surdo Mute|No_Snrs=Snare No Wires|"
    "Oct_Agg=Agogo Octave|Open_D1=Open Drum|Open_H2=Hat Open 2|Open_H4=Hat Open 4|Open_H5=Hat Open 5|Open_H6=Hat Open 6|"
    "Open_H7=Hat Open 7|Opn_Cui=Cuica Open|Opn_Pnd=Pandeiro Open|Opn_Srd=Surdo Open|Pedl_H2=Hat Pedal 2|Pedl_H4=Hat Pedal 4|"
    "Pedl_H5=Hat Pedal 5|Picl_S1=Piccolo Snare|Ping_S=Ping Snare|Pipe1=Pipe|Plate1=Plate|Punch_K=Punch Kick|"
    "Radio_S=Radio Snare|Rapnois=Rap Noise|Rdbl_C3=Ride Bell|Real_S=Real Snare|Real_T1=Real Tom 1|Real_T2=Real Tom 2|"
    "Real_T3=Real Tom 3|Real_T4=Real Tom 4|Ride_C3=Ride|Rimsht1=Rim Shot 1|Rimsht2=Rim Shot 2|Ring1=Ring|RivetRd=Rivet Ride|"
    "Rock_S1=Rock Snare|Rock_T1=Rock Tom 1|Rock_T2=Rock Tom 2|Rock_T3=Rock Tom 3|Rock_T4=Rock Tom 4|Roll_S1=Snare Roll 1|"
    "Roll_S3=Snare Roll 3|Room_K1=Room Kick 1|Room_K2=Room Kick 2|Room_K3=Room Kick 3|Room_K4=Room Kick 4|Room_T1=Room Tom 1|"
    "Room_T2=Room Tom 2|Room_T3=Room Tom 3|Room_T4=Room Tom 4|Rvb_Clp=Reverb Clap|Rvb_S1=Reverb Snare|Scrpull=Scratch Pull|"
    "Scrpush=Scratch Push|Shadow=Shadow|Sharp_S=Sharp Snare|Sho_Gui=Guiro Short|Sidstk1=Side Stick 1|Sidstk4=Side Stick 4|"
    "Slam_S=Slam Snare|Slap_Cg=Conga Slap|Slap_S1=Slap Snare 1|Slap_S3=Slap Snare 3|Slap_S4=Slap Snare 4|Slap_T1=Slap Tom 1|"
    "Slap_T2=Slap Tom 2|Slap_T3=Slap Tom 3|Slap_T4=Slap Tom 4|Slid_Cg=Conga Slide|Small_S=Small Snare|Snap_K=Snap Kick|"
    "Soft_K=Soft Kick|Solid_K=Solid Kick|Solid_S=Solid Snare|Spark1=Spark|Spla_C1=Splash 1|Spla_C2=Splash 2|Squas_=Squash|"
    "Steeldr=Steel Drum|Swis_S2=Swish Snare 2|Swis_S3=Swish Snare 3|Swis_S4=Swish Snare 4|Taiko2=Taiko|Tambrn1=Tambourine 1|"
    "Tambrn2=Tambourine 2|Tekno_K=Techno Kick|Thrillr=Thriller|Tight_S=Tight Snare|Timpan2=Timpani|Tmborim=Tamborim|"
    "Trash_S=Trash Snare|Triangl=Triangle|Vibslap=Vibraslap|Video_K=Video Kick|Video_S=Video Snare|Wblock=Wood Block 1|"
    "Wblock2=Wood Block 2|Wheel2=Wheel|Whistl1=Whistle 1|Whistl2=Whistle 2|Wood_K2=Wood Kick 2|Wood_K3=Wood Kick 3|"
    "Wood_S1=Wood Snare")

R70 = _pairs(
    "55Clav=55 Claves|60's S=60s Snare|606 T1=606 Tom 1|606 T2=606 Tom 2|606 T3=606 Tom 3|78Beat=78 Metal Beat|"
    "78Bngo=78 Bongo|78CHH=78 Hat Closed|78Cow=78 Cowbell|78Guir=78 Guiro|78Marac=78 Maracas|78OHH=78 Hat Open|"
    "78Tamb=78 Tambourine|808 T1=808 Tom 1|808 T2=808 Tom 2|808 T3=808 Tom 3|808CgH=808 Conga High|808CgL=808 Conga Low|"
    "808CHH=808 Hat Closed|808Clp=808 Clap|808Clv=808 Claves|808Cow=808 Cowbell|808GtK=808 Gated Kick|808Mrc=808 Maracas|"
    "808OHH=808 Hat Open|808Stk=808 Side Stick|90's S=90s Snare|909 Ack=909 Attack Kick|909CmK=909 Compressed Kick|"
    "AcouBs=Acoustic Bass|AgogoH=Agogo High|AgogoL=Agogo Low|AmbStk=Ambient Side Stick|BgShtS=Big Shot Snare|"
    "BngHMt=Bongo High Mute|BngHOp=Bongo High Open|BngLMt=Bongo Low Mute|BngLOp=Bongo Low Open|Boos T1=Boost Tom 1|"
    "Boos T2=Boost Tom 2|Boos T3=Boost Tom 3|BoSldK=Boost Solid Kick|Brite K=Bright Kick|BrRidC=Brush Ride|"
    "BrRlS1=Brush Roll Snare 1|BrRlS2=Brush Roll Snare 2|BrsCHH=Brush Hat Closed|BrSlS1=Brush Slap Snare 1|"
    "BrSlS2=Brush Slap Snare 2|BrSlS3=Brush Slap Snare 3|BrsOHH=Brush Hat Open|Brswi S=Brush Swirl Snare|"
    "BrtMdK=Bright Medium Kick|Brush T1=Brush Tom 1|Brush T2=Brush Tom 2|Brush T3=Brush Tom 3|CabaDn=Cabasa Down|"
    "CabaUp=Cabasa Up|Casta=Castanets|Chekre=Shekere|ChinaC=China|Chop S=Chop Snare|Claves=Claves|Click=Click|"
    "CmBrtK=Compressed Bright Kick|CngHmt=Conga High Mute|CngHop=Conga High Open|CngLMt=Conga Low Mute|CngLop=Conga Low Open|"
    "CngSlp=Conga Slap|Combo S=Combo Snare|Comrc K=Commercial Kick|CowblL=Cowbell Low|CowHMt=Cowbell High Mute|"
    "CowHOp=Cowbell High Open|CrashC1=Crash 1|CrashC2=Crash 2|CuicMt=Cuica Mute|CuicOp=Cuica Open|Dance S=Dance Snare|"
    "DblRn S=Double Ring Snare|Deep K=Deep Kick|DncClp=Dance Clap|Dopin S=Dopin Snare|DpRomK=Deep Room Kick|Dry K=Dry Kick|"
    "DryAcK=Dry Acoustic Kick|DryStk=Dry Side Stick|DryT1=Dry Tom 1|DryT2=Dry Tom 2|DryT3=Dry Tom 3|E2 K=E2 Kick|"
    "E2PlwK=E2 Pillow Kick|Fat S=Fat Snare|FingBs=Finger Bass|Gate K=Gated Kick|GuiroL=Guiro Long|GuiroS=Guiro Short|"
    "Hard K=Hard Kick|Hard S=Hard Snare|HIQ=High Q|HosDp S=House Deep Snare|House S=House Snare|Hybrd K=Hybrid Kick|"
    "KckAmb=Kick Ambience|LA S=LA Snare|LAFat S=LA Fat Snare|Latin S=Latin Snare|Lite S=Light Snare|LngAmb=Long Ambience|"
    "Loose S=Loose Snare|LtRvbK=Light Reverb Kick|Maracas=Maracas|MondoK=Mondo Kick|Noise S=Noise Snare|Pillw K=Pillow Kick|"
    "PndMte=Pandeiro Mute|PndOpn=Pandeiro Open|PndSlp=Pandeiro Slap|PopCHH=Pop Hat Closed|PopOHH=Pop Hat Open|"
    "PopPHH=Pop Hat Pedal|Power S=Power Snare|RchRm S=Rich Room Snare|Rckin S=Rockin Snare|RckLt S=Rock Light Snare|"
    "RckPw S=Rock Power Snare|RckSp S=Rock Special Snare|Real S=Real Snare|Real T1=Real Tom 1|Real T2=Real Tom 2|"
    "Real T3=Real Tom 3|RealCH=Real Hat Closed|RealOH=Real Hat Open|RealPH=Real Hat Pedal|Regae S=Reggae Snare|"
    "Rev Snare=Reverse Snare|Revrb K=Reverb Kick|RidBlC=Ride Bell|RideC=Ride|Rim T1=Rim Tom 1|Rim T2=Rim Tom 2|"
    "Rim T3=Rim Tom 3|Ring S=Ring Snare|Rock S=Rock Snare|Rock T1=Rock Tom 1|Rock T2=Rock Tom 2|Rock T3=Rock Tom 3|"
    "Rocker S=Rocker Snare|Room K=Room Kick|Room T1=Room Tom 1|Room T2=Room Tom 2|Room T3=Room Tom 3|"
    "RvSldK=Reverb Solid Kick|ScrPsh=Scratch Push|ScrPvl=Scratch Pull|Shaker=Shaker|SlapBs=Slap Bass|SlBell=Sleigh Bell|"
    "Smash K=Smash Kick|SmlHs S=Small House Snare|Snaps=Finger Snaps|Soft K=Soft Kick|Solid K=Solid Kick|SplashC=Splash|"
    "Splat S=Splat Snare|SprCb S=Super Combo Snare|Sticks=Sticks|Super S=Super Snare|SurdMt=Surdo Mute|SurdOp=Surdo Open|"
    "SurdRm=Surdo Rim|Swing S=Swing Snare|Talk S=Talk Snare|Tambrm=Tamborim|Tambrn=Tambourine|Tight S=Tight Snare|"
    "TimbiH=Timbale High|TimbiL=Timbale Low|TR808 K=TR-808 Kick|TR808 S=TR-808 Snare|TR909 K=TR-909 Kick|"
    "TR909 S=TR-909 Snare|Trimopn=Triangle Open|Trimte=Triangle Mute|VibSlp=Vibraslap|Video S=Video Snare|"
    "VrbClp=Reverb Clap|WhislL=Whistle Long|WhislS=Whistle Short|WoobL=Wood Block Low|WoodBH=Wood Block High")
# The R-70 set's folders of numbered sounds.
for _i in range(1, 29):
    R70[f"BD/kick - ({_i})"] = f"Kick {_i:02d}"
for _i in range(1, 9):
    R70[f"Cym/Cym - ({_i})"] = f"Cymbal {_i}"
for _i in range(1, 5):
    R70[f"Cym/Rev-Cym - ({_i})"] = f"Reverse Cymbal {_i}"
for _i in range(1, 16):
    R70[f"HiHat/HiHat - ({_i})"] = f"Hat {_i:02d}"
for _i in range(1, 41):
    R70[f"SD/snare - ({_i})"] = f"Snare {_i:02d}"
R70["SD/snr_040"] = "Snare 40 Alt"
R70["SD/clap - (1)"] = "Clap 1"
R70["SD/clap - (2)"] = "Clap 2"
R70["SD/snap"] = "Snap"
for _i in list(range(1, 40)) + [49]:
    R70[f"Tom/Tom - ({_i})"] = f"Tom {_i:02d}"
_PERC_WORDS = {"agogo": "Agogo", "Air": "Air", "bell": "Bell", "blip": "Blip", "Block": "Block", "bongo": "Bongo",
               "busha": "Busha", "can": "Can", "Clave": "Clave", "Congo": "Conga", "Cow": "Cowbell", "EFx": "FX",
               "Guiro": "Guiro", "rattle": "Rattle", "Rubme": "Rub", "scratch": "Scratch", "Shake": "Shaker",
               "SynCow": "Synth Cowbell", "Tamb": "Tambourine", "Tiaki": "Tiaki", "Timb": "Timbale", "TRI": "Triangle",
               "whistle": "Whistle"}
_PERC = ("Air_01a Air_01b Block_01a Block_01b Clave_01a Clave_01b Congo_01a Congo_01b Congo_02a Congo_02b Congo_03a "
         "Congo_03b Cow_01a Cow_01b Cow_02a Cow_02b EFx_01a EFx_01b EFx_01c Guiro_01a Guiro_01b Rubme_01a Rubme_01b "
         "Rubme_01c Shake_01a Shake_01b SynCow_01a SynCow_01b TRI_01a TRI_01b TRI_01c Tamb_01a Tamb_01b Tiaki_01a "
         "Tiaki_01b Timb_01a Timb_01b agogo_01a agogo_01b bell_01a bell_01b blip_01a blip_01b bongo_01a bongo_01b "
         "busha_01a busha_01b can_01a can_01b can_01c rattle_01a rattle_01b scratch_01a scratch_01b scratch_01c "
         "whistle_01a whistle_01b whistle_01c")
for _stem in _PERC.split():
    _word, _tail = _stem.split("_")
    R70["Perc/" + _stem] = f"Perc {_PERC_WORDS[_word]} {int(_tail[:2])}{_tail[2:].upper()}"
R70["Perc/shot_01"] = "Perc Shot"
R70["Perc/Vibra"] = "Perc Vibraslap"

MACHINES = {
    "Roland MC-202": ("Roland MC-202", MC202),
    "Roland MC-303": ("Roland MC-303", MC303),
    "Roland MC-808": ("Roland MC-808", MC808),
    "Roland R-8 MKII": ("Roland R8 MKII", R8),
    "Roland R-70": ("Roland R-70", R70),
}

# Playable kits (the layout of make_kits.py): name, machine, tempo, swing, groove, 16 pads, description.
KITS = [
    ("MC-202 Kit", "Roland MC-202", 124, 0.0, "bassline",
     ["Hit 1", "Hit 2", "Hit 4", "Hit 3", None, None, None, None,
      "Bass 1", "Bass 2", "Bass 3", "Bass 4", "Bass 5", None, None, None],
     "The MC-202's bass notes and hits (it is a bass synthesizer and sequencer): one-shots to play as a bassline."),
    ("MC-808 Kit", "Roland MC-808", 124, 0.0, "electro",
     ["Kick (01)", "Rim (07)", "Snare (04)", "Clap (06)", "Snare (05)", "Tom (59)", "Hat Closed (09)", "Tom (58)",
      "Hat Closed (11)", "Perc Tone (20)", "Hat Open (14)", "Kick (02)", "Perc Metal (10)", "Crash (22)", "Snare Short (08)", "Ride (23)"],
     "The MC-808 groovebox: punchy dance drums."),
    ("MC-808 Sub Kit", "Roland MC-808", 96, 0.0, "selector",
     ["808 Kick (35)", "808 Kick (36)", "Snare (05)", "Clap (06)", "Sub Kick (17)", "Sub Kick (18)", "Hat Closed (12)",
      "Sub Kick Long (19)", "Hat Closed (13)", "Sub Bass (31)", "Hat Open (15)", "Sub Bass (32)", "Sub Bass (33)",
      "Cymbal (16)", "Sub Bass (34)", "Hat Open (21)"],
     "The MC-808's 808 kicks and sub basses, with its other hats and cymbals."),
    ("MC-303 Kit", "Roland MC-303", 118, 0.0, "funk",
     ["Dry Kick 1", "Rim Shot", "Dry Snare 1", "Real Clap", "Tight Snare", "Acoustic Tom Low 1", "Real Hat Closed 1",
      "Acoustic Tom Mid 1", "Real Hat Pedal 1", "Acoustic Tom High 1", "Real Hat Open 1", "Conga Low Open", "Conga High Open",
      "Asian Gong", "Tambourine", "Ride 1"],
     "The MC-303 groovebox's acoustic-style kit."),
    ("MC-303 Vintage Kit", "Roland MC-303", 112, 0.0, "pop",
     ["606 Kick 1", "Tiny Rim 2", "606 Snare 1", "707 Clap", "78 Snare", "78 Tom Low", "606 Hat Closed", "78 Tom Mid",
      "78 Hat Closed", "78 Tom High", "707 Hat Open", "78 Guiro", "78 Metal Beat", "Ride 2", "78 Cowbell", "78 Tambourine"],
     "The MC-303's 606, 707 and CR-78 sounds."),
    ("MC-303 Jungle Kit", "Roland MC-303", 165, 0.0, "breakbeat",
     ["Jungle Kick 1", "Rim Snare 1", "Jungle Snare 1", "Rap Clap 1", "Jungle Snare 2", "Electric Tom Low 1",
      "Room Hat Closed", "Electric Tom Mid 1", "Real Hat Closed 2", "Electric Tom High 1", "Room Hat Open",
      "Jungle Snare 3", "Jungle Snare 4", "Shaker", "Jungle Tambourine", "Jungle Kick 2"],
     "The MC-303's jungle kicks and snares for breakbeats."),
    ("MC-303 Latin Kit", "Roland MC-303", 104, 0.0, "latin",
     ["Electric Kick", "Claves", "Hard Snare", "Hard Clap", "Cabasa Up", "Timbale Low", "Maracas", "Bongo Low Open",
      "Shaker Tambourine", "Bongo High Open", "Cabasa Down", "Conga High Slap", "Agogo High", "Agogo Low", "Cowbell", "Wood Block High"],
     "Congas, bongos, timbales, agogos and shakers from the MC-303."),
    ("R-8 MKII Kit", "Roland R-8 MKII", 112, 0.0, "pop",
     ["Room Kick 1", "Side Stick 1", "Real Snare", "Hand Clap", "Tight Snare", "Real Tom 4", "Hat Closed 2", "Real Tom 3",
      "Hat Pedal 2", "Real Tom 1", "Hat Open 2", "Conga Low", "Bongo High", "Crash 9", "Cowbell 1", "Ride"],
     "The R-8 MKII's studio kit: natural kick, snare and toms."),
    ("R-8 MKII Rock Kit", "Roland R-8 MKII", 120, 0.0, "rock",
     ["Hard Kick", "Rim Shot 1", "Rock Snare", "Dry Clap", "Fat Snare", "Rock Tom 4", "Hat Closed 4", "Rock Tom 3",
      "Hat Pedal 4", "Rock Tom 1", "Hat Open 4", "Rock Tom 2", "Gated Snare", "Crash 10", "China", "Ride Bell"],
     "Big rock drums: hard kick, rock snare and toms, gated snare."),
    ("R-8 MKII 808 Kit", "Roland R-8 MKII", 98, 0.0, "electro",
     ["808 Kick", "808 Side Stick", "808 Snare 2", "808 Clap", "Snap Kick", "808 Tom", "808 Hat Closed", "Doom Tom 1",
      "808 Maracas", "Doom Tom 2", "808 Hat Open", "808 Conga", "808 Claves", "Splash 1", "Cowbell 2", "Rivet Ride"],
     "The R-8 MKII's 808 card: kick, snare, clap, hats and toms of the TR-808."),
    ("R-8 MKII 909 Kit", "Roland R-8 MKII", 126, 0.0, "electro",
     ["909 Kick", "909 Side Stick", "909 Snare", "Reverb Clap", "House Snare", "909 Tom", "909 Hat Closed", "House Kick",
      "Hat Closed 5", "Techno Kick", "909 Hat Open", "Dance Snare", "Sharp Snare", "Crash 9", "High Q", "Splash 2"],
     "The R-8 MKII's 909 and house sounds."),
    ("R-8 MKII CR-78 Kit", "Roland R-8 MKII", 100, 0.18, "shuffle",
     ["78 Kick", "Side Stick 4", "78 Snare", "Finger Snap", "Light Snare", "Hollow Tom 4", "78 Hat Closed", "Hollow Tom 3",
      "78 Maracas", "Hollow Tom 1", "78 Hat Open", "78 Bongo", "78 Guiro", "Choke Cymbal", "78 Cowbell", "78 Tambourine"],
     "The R-8 MKII's CR-78 sounds: soft kick and snare, maracas, guiro and cowbell."),
    ("R-8 MKII Brush Kit", "Roland R-8 MKII", 88, 0.2, "ballad",
     ["Soft Kick", "Side Stick 1", "Swish Snare 2", "Finger Snap", "Swish Snare 3", "Slap Tom 4", "Brush Hat Closed",
      "Slap Tom 3", "Hat Pedal 5", "Slap Tom 1", "Brush Hat Open", "Swish Snare 4", "Snare Roll 1", "Splash 1", "Triangle", "Brush Ride"],
     "Brushes: swished snares, brush hats and ride, soft kick."),
    ("R-8 MKII Latin Kit", "Roland R-8 MKII", 104, 0.0, "latin",
     ["Floor Kick", "Claves", "Slap Snare 1", "Hand Clap", "Cabasa", "Timbale Low", "Maracas", "Bongo Low",
      "Tambourine 1", "Bongo High", "Castanets", "Conga Slap", "Conga Slide", "Agogo", "Cowbell 1", "Timbale High"],
     "The R-8 MKII's latin percussion."),
    ("R-70 Kit", "Roland R-70", 112, 0.0, "pop",
     ["Room Kick", "Dry Side Stick", "Real Snare", "Dance Clap", "Tight Snare", "Real Tom 3", "Real Hat Closed",
      "Real Tom 2", "Real Hat Pedal", "Real Tom 1", "Real Hat Open", "Conga Low Open", "Conga High Open", "Crash 1",
      "Tambourine", "Ride"],
     "The R-70's studio kit."),
    ("R-70 Rock Kit", "Roland R-70", 122, 0.0, "rock",
     ["Hard Kick", "Ambient Side Stick", "Rock Snare", "Reverb Clap", "Rock Power Snare", "Rock Tom 3", "Pop Hat Closed",
      "Rock Tom 2", "Pop Hat Pedal", "Rock Tom 1", "Pop Hat Open", "Rockin Snare", "Rocker Snare", "Crash 2", "China", "Ride Bell"],
     "Big rock and pop drums of the R-70."),
    ("R-70 808 Kit", "Roland R-70", 98, 0.0, "electro",
     ["TR-808 Kick", "808 Side Stick", "TR-808 Snare", "808 Clap", "808 Gated Kick", "808 Tom 3", "808 Hat Closed",
      "808 Tom 2", "808 Maracas", "808 Tom 1", "808 Hat Open", "808 Conga Low", "808 Conga High", "Splash", "808 Cowbell", "808 Claves"],
     "The R-70's TR-808 sounds."),
    ("R-70 909 Kit", "Roland R-70", 126, 0.0, "electro",
     ["TR-909 Kick", "Sticks", "TR-909 Snare", "Dance Clap", "House Snare", "Boost Tom 3", "Hat 01", "Boost Tom 2",
      "Hat 02", "Boost Tom 1", "Hat 03", "909 Attack Kick", "909 Compressed Kick", "Cymbal 1", "High Q", "Cymbal 2"],
     "The R-70's TR-909 and dance sounds."),
    ("R-70 Vintage Kit", "Roland R-70", 100, 0.16, "shuffle",
     ["Soft Kick", "55 Claves", "60s Snare", "Finger Snaps", "90s Snare", "606 Tom 3", "78 Hat Closed", "606 Tom 2",
      "78 Maracas", "606 Tom 1", "78 Hat Open", "78 Bongo", "78 Guiro", "78 Metal Beat", "78 Cowbell", "78 Tambourine"],
     "CR-78 and TR-606 sounds, 60s and 90s snares."),
    ("R-70 Brush Kit", "Roland R-70", 88, 0.2, "ballad",
     ["Pillow Kick", "Dry Side Stick", "Brush Slap Snare 1", "Brush Roll Snare 1", "Brush Swirl Snare", "Brush Tom 3",
      "Brush Hat Closed", "Brush Tom 2", "Brush Slap Snare 2", "Brush Tom 1", "Brush Hat Open", "Brush Slap Snare 3",
      "Brush Roll Snare 2", "Splash", "Triangle Open", "Brush Ride"],
     "The R-70's brushes: slaps, rolls, swirls, brush hats and ride."),
    ("R-70 Latin Kit", "Roland R-70", 104, 0.0, "latin",
     ["Dry Kick", "Claves", "Latin Snare", "Dance Clap", "Cabasa Up", "Surdo Open", "Shaker", "Conga Low Mute",
      "Shekere", "Conga High Mute", "Cabasa Down", "Conga Slap", "Bongo High Open", "Agogo High", "Cowbell High Open", "Timbale High"],
     "The R-70's latin percussion: surdo, congas, bongos, agogos, shekere and more."),
]
