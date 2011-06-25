module Red2.Reduceron where

import Lava
import Monad
import CircLib.Bit
import CircLib.Word
import CircLib.Common
import CircLib.RTL
import CircLib.OffsetReg
import Red2.ProcState
import Red2.Mem
import Red2.Bytecode

reduceron :: ProcState -> RTL ()
reduceron s =  initialise s >> loop (step s)

initialise :: ProcState -> RTL ()
initialise s =  do reg (hp s) <-- address (hpInit s)
                   reg (sp s) <-- maxAddr
                   top s <-- mkFunNode (word 0)
                   tick

step :: ProcState -> RTL ()
step s =  do t <- readVar (top s)

             let fun  = isFunNode t
             let func = fun <&> inv (isPrim t)
             let prim = fun <&> isPrim t

             choose
               [ isIntNode t --> handleInt s t
               , prim        --> handlePrim s t
               , func        --> handleFun s t
               , isApNode t  --> do increment (sp s) 1 ; unwind s t
               ]

handleInt :: ProcState -> Word -> RTL ()
handleInt s t = do stackPtr <- readVar (reg (sp s))

                   let fin = tree (<&>) stackPtr

                   choose
                     [ fin     --> do r0 s <-- (t `shre` 3)
                                      halted s <-- [high]
                                      stop
                     , inv fin --> do x <- readMemOff s (sp s, 1)
                                      tick
                                      writeMemOff s t (sp s, 1)
                                      tick
                                      top s <-- x
                                      tick
                     ]

handlePrim :: ProcState -> Word -> RTL ()
handlePrim s t = do x <- readMemOff s (sp s, 1)
                    tick
                    y <- readMemOff s (sp s, 2)
                    tick
                    r1 s <-- (x `shre` 3)
                    z <- readMemOff s (sp s, 3)
                    tick

                    a <- readVar (r1 s)
                    let b = y `shre` 3

                    let resEq = a /=/ b
                        resNotEq = inv resEq
                        resLessEq = a /<=/ b
                        arith = isAdd t <|> isSub t
                        res = isAdd t ? (a /+/ b, a /-/ b)

                    let resTrue  =  (isEq t <&> resEq)
                                <|> (isNotEq t <&> resNotEq)
                                <|> (isLessEq t <&> resLessEq)

                    choose
                      [ arith -->
                          do increment (sp s) 2
                             top s <-- mkIntNode res
                             tick
                      , inv arith -->
                          choose
                            [ resTrue -->
                                do tick
                                   increment (sp s) 4
                                   top s <-- z
                                   tick
                            , inv resTrue -->
                                do val <- readMemOff' s (sp s, 4)
                                   tick
                                   tick
                                   top s <-- val
                                   tick
                            ]
                      ]

handleFun :: ProcState -> Word -> RTL ()
handleFun s t = unfold s t

unwind :: ProcState -> Word -> RTL ()
unwind s t = do let topAddr = drop 3 t
                a0 s <-- (topAddr /+/ (1 `ofWidth` addrWidth))
                addr <- readVar (a0 s)
                t <- readVar (top s)
                let notEnd = inv (isEnd t)
                stkPtr <- readVar (reg (sp s))
                val <- readMem s topAddr
                tick
                tick

                doWhile notEnd $
                  do readMem s addr
                     a0 s <-- (addr /+/ (1 `ofWidth` addrWidth))
                     top s <-- val
                     increment (sp s) (-1)
                     tick
                     onlyIf notEnd $
                       do writeMem s t stkPtr
                          tick

trAndCopy :: ProcState -> Word -> RTL ()
trAndCopy s node = do let end = isEnd node
                      bit0 s <-- [end]
                      end' <- readVar (bit0 s)
                      stkPtr <- readVar (reg (sp s))
                      base <- readVar (a1 s)

                      let argAddr = stkPtr /+!/ getArg node
                          ap      = (base /+/ getAp node)
                                          /-/  (1 `ofWidth` addrWidth)
                          isArg   = isArgNode node
                          isAp    = isApNode node
                          isOther = isFunNode node <|> isIntNode node


                      choose
                        [ isArg -->
                            do arg <- readMem s argAddr
                               tick
                               bit1 s <-- [high]
                               r1 s <-- arg
                               tick
                               increment (hp s) 1
                               writeMemOff s (markEnd end' arg) (hp s, 0)
                               tick
                        , isAp -->
                            do increment (hp s) 1
                               let apNode = mkApNode ap
                               writeMemOff s (markEnd [end] apNode) (hp s, 0)
                               tick
                        , isOther -->
                            do increment (hp s) 1
                               writeMemOff s node (hp s, 0)
                               tick
                        ]



unfold :: ProcState -> Word -> RTL ()
unfold s top = do let initialAddr = drop 4 top ++ [low]
                  
                  a0 s <-- (initialAddr /+/ (1 `ofWidth` addrWidth))
                  addr <- readVar (a0 s)
                  val <- readVar (r1 s)
                  stall <- liftM head (readVar (bit1 s))
                  bit1 s <-- [low]

                  heapPtr <- readVar (reg (hp s))
                  a1 s <-- heapPtr

                  start <- readMem s initialAddr
                  tick

                  node <- readMem s addr
                  a0 s <-- (addr /+/ (1 `ofWidth` addrWidth))
                  tick

                  (numArgs, spineLen, size) <- extract start
                  tick

                  while (tree (<|>) size) $
                    do readMem s addr
                       a0 s <-- (addr /+/ (1 `ofWidth` addrWidth))
                       choose
                         [ stall --> (bit1 s <-- [low])
                         , inv stall --> (r1 s <-- node)
                         ]
                       tick
                       dec0 s <-- (size /-/ (1 `ofWidth` 10))
                       trAndCopy s val

                  let lastAddr = heapPtr /-/ (spineLen ++ replicate 11 low)
                  reg (hp s) <-- lastAddr

                  stkPtr <- readVar (reg (sp s))
                  reg (sp s) <-- (stkPtr /+!/ (numArgs ++ replicate 11 low))

                  unwind s (replicate 3 low ++ lastAddr)
  where
    extract start = do nib0 s <-- take 4 start
                       nib1 s <-- drop 14 start
                       dec0 s <-- take 10 (drop 4 start)
                       numArgs <- readVar (nib0 s)
                       spineLen <- readVar (nib1 s)
                       size <- readVar (dec0 s)
                       return (numArgs, spineLen, size)
