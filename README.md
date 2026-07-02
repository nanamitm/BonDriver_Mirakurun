# BonDriver_Mirakurun

TVTestから[Mirakurun](https://github.com/kanreisa/Mirakurun)を利用する為のBonDriverです。

ソースコードのベースはBonDriver_HTTPからです。  
Visual Studio 2022 (v143ツールセット) でビルドしています。x64/Win32いずれの構成もビルド可能ですが、
4K/8K(MMT/TLV)対応はx64ビルドのみです(詳細は後述)。

## ランタイム
[Microsoft Visual C++ 2015-2022 Redistributable]を必要とします  

Microsoft公式よりランタイムのダウンロードおよびインストールを行ってください  
https://support.microsoft.com/ja-jp/help/2977003/the-latest-supported-visual-c-downloads

## 4K/8K(MMT/TLV)対応

[Mirakurun-BS4K](https://github.com/tsuyopon123/Mirakurun-BS4K)のようにBS4K等のMMT/TLVチャンネルを
配信するMirakurunフォークに対しても、通常のGR/BS/CSと同じチューナー・同じチャンネル一覧の中から
シームレスに視聴できます。裏側では[dantto4k](https://github.com/nanamitm/dantto4k)(MMTS保存対応フォーク)のMMT/TLVデマルチプレクサ・
ACAS復号・MPEG2-TSリマルチプレクサのコードをgit submodule(`thirdparty/dantto4k`)として取り込み、
ビルド時に本体へ静的リンクしています(別プロセスやサブプロセスは使いません)。

Mirakurunの`/api/channels`または`/api/services`が返すチャンネルの`type`が`MMT_TYPES`(既定`BS4K`)に
一致する場合のみ、内部でMMT/TLV→MPEG2-TS変換を行います。それ以外のチャンネル(GR/BS/CSなど)は
今まで通り生のTSをそのまま返します。

### セットアップ

```
git submodule update --init --recursive
```

でdantto4k本体とその依存(asio、tsduck)を取得してからビルドしてください。初回ビルド時にTSDuckの
静的ライブラリ(`thirdparty/dantto4k/thirdparty/tsduck`)を自動的にビルドします(10分程度)。

この機能はx64ビルドのみ対応です(TSDuckの静的ライブラリをx64向けにしかビルドしていないため)。
Win32(x86)ビルドではMMT/TLV変換は無効になり、従来通りGR/BS/CS等のTS配信のみ利用できます。

ACAS復号にはローカルのB-CASカードリーダー、またはリモートの
[CasProxyServer](https://github.com/nekohkr/casproxyserver)が必要です。iniの`[MMT4K]`セクションで設定してください。

### MMTS保存 (EDCB / Write_MMTS連携)

[dantto4k](https://github.com/nanamitm/dantto4k)と同じ`StartMmtsRecording`/`StopMmtsRecording`/
`GetMmtsRecordingStatus`をエクスポートしています。EDCBで
[Write_MMTS](https://github.com/nanamitm/Write_MMTS)プラグインを使うと、MMT/TLV(4K/8K)チャンネルを
MPEG2-TSに変換せず、受信channelのstreamをACASデスクランブル解除のみ行った状態で`.mmts`(+索引用の
`.mmtsmap`)としてそのまま保存できます。GR/BS/CS等の通常チャンネルは今まで通りWrite_MMTSが
`.ts`として保存します(dantto4k未ロード時と同じフォールバック動作)。

## License
This software is released under the MIT License, see LICENSE.
