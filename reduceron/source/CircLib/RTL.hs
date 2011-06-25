module CircLib.RTL where

import Lava
import CircLib.Bit
import CircLib.Word
import CircLib.Common hiding ((!))
import Data.Map hiding (map)

data RTL a        =  RTL { run :: Bit -> Env -> (Bit, Env, a) }

instance Monad RTL where
  return a        =  RTL $ \start env -> (start, env, a)
  m >>= f         =  RTL $ \start env ->
                       let (fin0, env0, a) = run m start env
                           (fin1, env1, b) = run (f a) fin0 env0
                       in  (fin1, env1, b)

data Var          =  Var { ident :: Int
                         , width :: Int
                         }

type Inp          =  (Bit, Word)

data Out          =  Out { newOut :: Word
                         , oldOut :: Word
                         }

data Env          =  Env { freshId    :: Int
                         , readerInps :: Map Int [Inp]
                         , writerInps :: Map Int [Inp]
                         , outs       :: Map Int Out
                         }

rtl               :: Bit -> RTL a -> (Bit, a)
rtl start m       =  (fin, a)
  where
    (fin, env, a) =  run m start (Env 0 (writerInps env) empty empty)

newOutput env a   =  (v, env')
  where
    v             =  freshId env
    env'          =  env { freshId = v + 1
                         , outs    = insert v a (outs env)
                         }

addInps env as    =  env { writerInps = inps }
  where
    inps          =  unionWith (++) (fromList as) (writerInps env)

getInps env v w   =  case Data.Map.lookup v env of
                       Nothing -> [(low, replicate w low)]
                       Just inps -> inps

newSig            :: Int -> RTL Var
newSig width      =  RTL $ \start env ->
                       let (v, env') = newOutput env (Out out' out')
                           inps      = getInps (readerInps env) v width
                           out       = uncurry select (unzip inps)
                           out'      = lazyZipWith (\a b -> b) 
                                         (replicate width ()) out
                       in  (start, env', Var v width)

newReg            :: Int -> RTL Var
newReg width      =  RTL $ \start env ->
                       let (v, env') = newOutput env (Out value' out)
                           inps      = getInps (readerInps env) v width
                           init      = replicate width False
                           out       = rege init enable value'
                           enable    = tree (<|>) (map fst inps)
                           value     = uncurry select (unzip inps)
                           value'    = lazyZipWith (\a b -> b) 
                                         (replicate width ()) value
                       in  (start, env', Var v width)

tick              :: RTL ()
tick              =  RTL $ \start env -> (delay low start, env, ())

skip              :: RTL ()
skip              =  RTL $ \start env -> (start, env, ())

loop              :: RTL () -> RTL ()
loop p            =  RTL $ \start env ->
                       let (fin, env', b) = run p loopBit env
                           loopBit        = start <|> fin
                       in  (low, env', b)

while             :: Bit -> RTL a -> RTL a
while c p         =  RTL $ \start env ->
                       let (fin, env', b) = run p (c <&> loopBit) env
                           loopBit        = start <|> fin
                       in  (inv c <&> loopBit, env', b)

doWhile           :: Bit -> RTL a -> RTL a
doWhile c p       =  RTL $ \start env ->
                       let (fin, env', b) = run p loopBit env
                           loopBit        = start <|> (fin <&> c)
                       in  (fin <&> inv c, env', b)

readVar           :: Var -> RTL Word
readVar v         =  RTL $ \start env ->
                       (start, env, oldOut (outs env ! ident v))

readNew           :: Var -> RTL Word
readNew v         =  RTL $ \start env ->
                       (start, env, newOut (outs env ! ident v))

writeVar          :: Var -> Word -> RTL ()
writeVar v a      =  case width v == length a of
                       False -> error message
                       True  -> RTL $ \start env ->
                                  let inp = (ident v, [(start, a)])
                                  in  (start, addInps env [inp], ())
  where
    message       =  "RTL.writeVar: assigning a value of width "
                  ++ show (length a) ++ " to a variable of width "
                  ++ show (width v)

(<--)             :: Var -> Word -> RTL ()
v <-- w           =  writeVar v w

choose            :: [(Bit, RTL ())] -> RTL ()
choose as         =  RTL $ \start env -> 
                       let as' = map (\(s, m) -> (start <&> s, m)) as
                       in  f env [] as'
  where
    f e fins []          = (tree (<|>) fins, e, ())
    f e fins ((s, m):as) = case run m s e of
                             (fin, e', _) -> f e' (fin:fins) as

onlyIf c a        =  choose [ c --> a, inv c --> skip ]

(-->)             :: a -> b -> (a, b)
a --> b           =  (a, b)

stop              :: RTL ()
stop              =  RTL $ \start env -> (low, env, ())

getStart          :: RTL Bit
getStart          =  RTL $ \start env -> (start, env, start)

setStart          :: Bit -> RTL ()
setStart go       =  RTL $ \start env -> (go, env, ())

data Label        =  Label { labelStart :: Var, labelFin :: Bit }

share             :: RTL () -> RTL Label
share f           =  do go <- newSig 1
                        s <- getStart
                        [g] <- readVar go
                        setStart g
                        f
                        fin <- getStart
                        setStart s
                        return (Label { labelStart = go, labelFin = fin })

call              :: Label -> RTL ()
call l            =  do labelStart l <-- [high]
                        setStart (labelFin l)
