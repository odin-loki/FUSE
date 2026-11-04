"""Rebuild NotoSansCJKsc-Subset.otf (see VERSION for the source font and its sha256).

usage: python make_cjk_subset.py NotoSansCJKsc-Regular.otf NotoSansCJKsc-Subset.otf

Needs fontTools 4.63 (pyftsubset). The output is byte-for-byte deterministic: pyftsubset keeps the
source head.created / head.modified, so nothing depends on the build time.
"""
import os
import sys
import tempfile

from fontTools import subset

# Kana, CJK symbols and punctuation, katakana phonetic extensions, half- and fullwidth forms.
RANGES = [(0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF), (0x31F0, 0x31FF), (0xFF00, 0xFFEF)]

# Hangul: conjoining jamo (composed by the ljmo/vjmo/tjmo features) and compatibility jamo.
HANGUL_RANGES = [(0x1100, 0x11FF), (0x3131, 0x318E)]

# 1382 frequent Han characters: Unihan 15.1 kTGH index <= 1000 or kFrequency <= 2.
HAN = """
    一丁七万丈三上下不与丐丑专且世丘丙业丛东丝丢两严並个丫中丰串丸丹为主丽乃久么义之乌乍乎乏乐乒乓乔乙九
    乞也习乡书买乱了予争事二于亏云互五井亚些亡亢交亥亦产亩人亿什仁仅仆仇今介仍从仑仓仔他仗付仙代令以仪们
    仰仲件价任份仿企伊伍伏伐休众优伙会伞伟传伤伦伪伯估伴伶伸伺似佃但位低住佐佑体何余佛作你佣使來便保信修
    個們候做備傳像價儿允元兄充兆先光克免兑兒入內全兩八公六兰共关兴兵其内冈冉册再冗写军农冬冯冰冲决况冶冷
    冻几凡凤凰凶凸凹出击刀刁刃分切刊刑划列刘则刚创初删判別刨利别到制則前力劝办功加务劣动助努劫励劲劳势動
    務勺勾勿匀包匆匈匕化北匠匣匹区医區十千升午卉半华协单南卜占卡卢卤卦卫卯印危即却卵卻厂厅历厉压厌原去县
    参參又叉及友双反发取受变口古句另叨叩只叫召叭叮可台史右叶号司叹叼叽吁吃各吆合吉吊同名后吏吐向吓吕吗君
    吝吞吟吠否吧吨吩含听吭吮启吱吴吵吸吹吻吼吾呀呆呈告呐呕员呛呜呢呵命和品哈員哪啊問啦喔喜單嗎器囚四回因
    团囤园困囱围国图國圖土圣在地场圾址均坊坎坏坐坑块坚坛坝坟坠坡坤坦坪坯坷垃型城基報場士壬壮声壳处备夕外
    多夢大天太夫夭央夯失头夷夸夹夺奇奉女奴奶奸她好如妃妄妆妇妈妒妓妖妙妥妨妳姊始子孔孕字存孙孝学孩學宁它
    宅宇守安宋完宏定实室家容實寫寸对寺寻导寿将將專對小少尔尖尘尤尧尬就尸尺尼尽尾尿局屁层屯山屹屿岁岂岔岖
    岗岛川州巡工左巧巨巩巫差己已巳巴巾币市布帅帆师希帐带師帮帶常幫干平年并幸幻幼幾广庄庆庇床序庐库应度延
    廷建开异弃弄式弓引弗弘弛弟张張強归当形彤影役彻很後得從心必忆忌忍志忘忙忧快忱念怀怎思性怪总您情想意愛
    感應戈戊戌戎戏成我戒或战戰户所手才扎扑扒打扔托扛扣执扩扫扬扭扮扯扰扳扶批扼找技抄把抑抒抓投抖抗折抚抛
    抠抡抢护报披抬抱抵抹押抽拂拄担拆拇拉拌拍拎拐拒拓拔拖拗拘拙招拟拢拣拥拦拧拨择拿持指换掉接提換支收改攻
    放政故教数整數文斗料斤斥新方於无日旦旧旨早旬旭旱时旷明昔星是時曰曲更書曾最會月有朋服望期木未末本术朱
    朴朵机朽杀杂权杆杉李杏材村杖杜束杠条来杨東板极林果标校样格案档梦業樂標樣機檔權欠次欢歌歡止正此步武歹
    死歼母每比毕毛氏民气氣水永汁求汇汉汗汛汝汞江池污汤汪汰汹決汽沁沃沈沉沐沒沙沛沟没沥沦沧沪法泛活流海清
    湾满滿灣火灭灯灰灵灶灸灼灾灿点為無然爪爱爲父爷片版牙牛牡牢物特犬犯状犹狂狈独獨玄玉王玖玛玩玫环现班現
    球理瓜瓦甘生用甩甫田由甲申电男甸画界留畫當疗發白百皂的皮皿目盯直相看真眼矛矢矣知石研破確示礼社祀神禾
    秀私秃科程種穴究穷空立站章竹笑第等算管米类系級組結給統經網線總纠红纤约级纪纫纬纯纱纲纳纵纷纸纹纺纽组
    经结给统网罕羊美義羽老考者而耳耶联聯聲聽肉肋肌肖肘肚肛肝肠能脑腦臣自至臺臼與興舌舟般良色艺艾节芋芒芙
    芜芝芥芦芬芭芯花芳芹芽苇苍苏若苦英茉華萬著處號虫虽血行衣补表被装裝裡西要見視覺觀见观规视觉角解言計訊
    討記設許訴試話該認語說誰課請論講謝識議讀變讓计订认讥讨让训议讯记讲讳讶许讹论讼讽设访诀证评识诈诉诊词
    译试话该语说请读课谁谢谷豆象貓買費資賽贝贞负贡财责费资赛赤走起超足跟路身車軍較轉车轧轨轩转较辛辦辰边
    辽达迁迂迄迅过迈迎运近返还这进远违连迟迷选這通速造連進運過道遠選還邊邑邓那邦邪邮邻部都酉里重量金錄錢
    錯针钉钱错長长門開間關门闪闭问闯闰闲间闷队防阳阴阵阶阻阿附际陆陈除陳隊难雖難電需青非面韧音頭題類页顶
    题風风飛飞饥饭饮馬驗马驮驯驰驱驳驴验體高鳳鸟鸡麦麼黃點黨齐龍龙龟
"""


def ks_x_1001_syllables():
    # The 2350 precomposed Hangul syllables of KS X 1001 (EUC-KR rows 0xB0-0xC8).
    out = []
    for hi in range(0xB0, 0xC9):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(ord(bytes([hi, lo]).decode("euc_kr")))
            except UnicodeDecodeError:
                pass
    return out


def code_points():
    cps = set()
    for lo, hi in RANGES + HANGUL_RANGES:
        cps.update(range(lo, hi + 1))
    cps.update(ord(c) for c in HAN if not c.isspace())
    cps.update(ks_x_1001_syllables())
    return sorted(cps)


def main(src, dst):
    cps = code_points()
    with tempfile.TemporaryDirectory() as tmp:
        unicodes = os.path.join(tmp, "unicodes.txt")
        with open(unicodes, "w") as f:
            f.write(",".join("%X" % u for u in cps))
        subset.main([src, "--unicodes-file=" + unicodes, "--output-file=" + dst,
                     "--layout-features=kern,palt,halt,ljmo,vjmo,tjmo", "--no-hinting",
                     "--drop-tables+=VORG,vhea,vmtx,BASE"])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
