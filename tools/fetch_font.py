"""Descarga verificada de Noto Sans Mono CJK JP (o SC con --sc) (OFL); nunca instala la fuente."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
COMMIT='f8d157532fbfaeda587e826d4cd5b21a49186f7c'
BASE=f'https://raw.githubusercontent.com/notofonts/noto-cjk/{COMMIT}/Sans/'
JP=('Mono/NotoSansMonoCJKjp-Regular.otf','NotoSansMonoCJKjp-Regular.otf',
    '4d01725be822d144cf9a56ade981e6fb920cd7a610b8fc24cc601a920beea5b9')
SC=('Mono/NotoSansMonoCJKsc-Regular.otf','NotoSansMonoCJKsc-Regular.otf',  # build_vita.py --gbk
    'ec04cc376b34887cedbdf84074e2e226ed2761eeabdcb9173fc1dd7bfd153ef7')
LICENSE=('LICENSE','NotoCJK-OFL.txt','6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2')

def fetch(directory,check=False,files=(JP,LICENSE)):
    directory=Path(directory)
    for remote,name,expected in files:
        target=directory/name
        if target.exists():
            data=target.read_bytes()
        else:
            if check: raise ValueError(f'Falta {target}')
            with urllib.request.urlopen(BASE+remote,timeout=60) as response:
                data=response.read()
        actual=hashlib.sha256(data).hexdigest()
        if actual!=expected: raise ValueError(f'SHA-256 incorrecto: {target}: {actual}')
        if not target.exists():
            directory.mkdir(parents=True,exist_ok=True)
            target.write_bytes(data)
        print(f'OK {name}: {len(data)} bytes, SHA-256 {actual}')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=Path,default=ROOT/'local/fonts')
    parser.add_argument('--check',action='store_true',help='Verificar sin acceso a red')
    parser.add_argument('--sc',action='store_true',help='Noto Sans Mono CJK SC (chino simplificado), para build_vita.py --gbk')
    args=parser.parse_args()
    try:fetch(args.output_dir,args.check,(SC if args.sc else JP,LICENSE))
    except (ValueError,OSError) as error:parser.exit(1,str(error)+'\n')

if __name__=='__main__':main()
