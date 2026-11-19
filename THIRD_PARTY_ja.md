# 同梱ライブラリと対応ソース

Scheme CD Ripper本体はMITライセンスです。CLIの画像表示には、次の共有ライブラリを同梱しています。

## Chafa

Chafa 1.14.2は、Copyright (C) 2018-2023 Hans Petter Jansson、GNU Lesser General Public License version 3以降（LGPL-3.0-or-later）です。
[上流のソース](https://github.com/hpjansson/chafa/tree/1.14.2)を`deps/chafa`で固定し、変更せずにライブラリだけをビルドします。

パッケージ内のライブラリは`/usr/lib/cdrip/libchafa.so.0`です。
ライセンス全文は`/usr/share/doc/cdrip/chafa/COPYING`（GPLv3）と`COPYING.LESSER`（LGPLv3）にあります。
ソース配布物では、同じ文書が`deps/chafa/`に入っています。
オンラインでは、上流の[GPLv3](https://github.com/hpjansson/chafa/blob/1.14.2/COPYING)と[LGPLv3](https://github.com/hpjansson/chafa/blob/1.14.2/COPYING.LESSER)を参照してください。

Chafaにはsmolscaleが含まれます。Copyright © 2019-2023 Hans Petter Jansson。
同梱版は、上流が認める選択肢のうちLGPLv3に従って配布します。
著作権・ライセンス文書は`/usr/share/doc/cdrip/chafa/smolscale/`に、[元の文書](https://github.com/hpjansson/chafa/tree/1.14.2/chafa/internal/smolscale)をそのまま収録しています。

## ソースの入手と再ビルド

バイナリを配布する際は、同じビルドで生成した`cdrip-<version>-source.tar.xz`も同じ配布場所で公開してください。
このアーカイブには、ビルドに使用したChafaとsmolscaleのソース、Scheme CD Ripperのソース、ビルド用スクリプトが含まれます。
`build.sh`は`build/deb/`（`BUILD_DIR`指定時はその配下の`deb/`）、`build_package.sh`は`artifacts/source/`に生成します。
[公式リリースの配布場所](https://github.com/kekyo/scheme-cd-ripper/releases)では、使用しているバイナリと同じ版のアーカイブを選んでください。
GitHubが自動生成するSource codeアーカイブには、submoduleの内容が含まれないため、代用できません。

空のディレクトリに展開し、READMEのビルド依存パッケージを導入してください。
Gitの履歴がないソース配布物では、配布物の版番号を`CDRIP_PACKAGE_VERSION`環境変数に設定して`./build.sh`を実行します。
Chafaのソースを改変した場合も、同じ手順で共有ライブラリを再ビルドできます。

## ライブラリの差し替え

インターフェイス互換の改変版`libchafa.so.0`を任意のディレクトリに置き、次のように指定してください。
CLIを再リンクする必要はありません。

```bash
LD_LIBRARY_PATH=/path/to/modified-chafa/lib cdrip
```

Chafaの改変と、その改変をデバッグするためのリバースエンジニアリングを制限しません。
Chafaには、GPLv3・LGPLv3の定める範囲で保証がありません。詳細は同梱のライセンス全文を参照してください。
