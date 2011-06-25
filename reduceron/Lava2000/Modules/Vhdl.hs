module Vhdl
  ( writeVhdl
  , writeVhdlInput
  , writeVhdlInputOutput
  )
 where

import Signal
import Netlist
import Generic
import Sequent
import Error
import LavaDir

import List
  ( intersperse
  , nub
  )

import IO
  ( openFile
  , IOMode(..)
  , hPutStr
  , hClose
  )

import System.IO
  ( stdout
  , BufferMode (..)
  , hSetBuffering
  )

import Data.IORef

import System
  ( system
  , ExitCode(..)
  )

----------------------------------------------------------------
-- write vhdl

writeVhdl :: (Constructive a, Generic b) => String -> (a -> b) -> IO ()
writeVhdl name circ =
  do writeVhdlInput name circ (var "inp")

writeVhdlInput :: (Generic a, Generic b) => String -> (a -> b) -> a -> IO ()
writeVhdlInput name circ inp =
  do writeVhdlInputOutput name circ inp (symbolize "outp" (circ inp))

writeVhdlInputOutput :: (Generic a, Generic b)
                     => String -> (a -> b) -> a -> b -> IO ()
writeVhdlInputOutput name circ inp out =
  do writeItAll name inp (circ inp) out

writeItAll :: (Generic a, Generic b) => String -> a -> b -> b -> IO ()
writeItAll name inp out out' =
  do hSetBuffering stdout NoBuffering
     putStr ("Writing to file \"" ++ file ++ "\" ... ")
     writeDefinitions file name inp out out'
     putStrLn "Done."
 where
  file = name ++ ".vhd"

----------------------------------------------------------------
-- definitions

writeDefinitions :: (Generic a, Generic b)
                 => FilePath -> String -> a -> b -> b -> IO ()
writeDefinitions file name inp out out' =
  do firstHandle  <- openFile firstFile WriteMode
     secondHandle <- openFile secondFile WriteMode
     thirdHandle  <- openFile thirdFile WriteMode
     var <- newIORef 0

     hPutStr firstHandle $ unlines $
       [ "-- Generated by Lava 2000"
       , ""
       , "library IEEE;"
       , "use IEEE.STD_LOGIC_1164.ALL;"
       , "use IEEE.STD_LOGIC_ARITH.ALL;"
       , "use IEEE.STD_LOGIC_UNSIGNED.ALL;"
       , ""
       , "library unisim;"
       , "use unisim.vcomponents.all;"
       , ""
       , "use work.all;"
       , ""
       , "entity"
       , "  " ++ name
       , "is" 
       , "port"
       , "  -- clock"
       , "  ( " ++ "clk" ++ " : in std_logic"
       , ""
       , "  -- inputs"
       ] ++
       [ "  ; " ++ v ++ " : in std_logic"
       | VarBool v <- inps
       ] ++
       [ ""
       , "  -- outputs"
       ] ++
       [ "  ; " ++ v ++ " : out std_logic"
       | VarBool v <- outs'
       ] ++
       [ "  );"
       , "end entity " ++ name ++ ";"
       , ""
       , "architecture"
       , "  structural"
       , "of"
       , "  " ++ name
       , "is"
       ]

     hPutStr secondHandle $ unlines $
       [ "begin"
       ]

     hPutStr thirdHandle $ unlines $
       [ "  attribute INIT : string;"
       ]


     let new =
           do n <- readIORef var
              let n' = n+1; v = "w" ++ show n'
              writeIORef var n'
              hPutStr firstHandle ("  signal " ++ v ++ " : std_logic;\n")
              return v

         define v s =
           case s of
             Bool True     -> port "vcc"  []
             Bool False    -> port "gnd"  []
             Inv x         -> port "inv"  [x]

             And []        -> define v (Bool True)
             And [x]       -> port "id"   [x]
             And [x,y]     -> port "and2" [x,y]
             And (x:xs)    -> define (w 0) (And xs)
                           >> define v (And [x,w 0])

             Or  []        -> define v (Bool False)
             Or  [x]       -> port "id"   [x]
             Or  [x,y]     -> port "or2"  [x,y]
             Or  (x:xs)    -> define (w 0) (Or xs)
                           >> define v (Or [x,w 0])

             Xor  []       -> define v (Bool False)
             Xor  [x]      -> port "id"   [x]
             Xor  [x,y]    -> port "xor2" [x,y]
             Xor  (x:xs)   -> define (w 0) (Or xs)
                           >> define (w 1) (Inv (w 0))
                           >> define (w 2) (And [x, w 1])

                           >> define (w 3) (Inv x)
                           >> define (w 4) (Xor xs)
                           >> define (w 5) (And [w 3, w 4])
                           >> define v     (Or [w 2, w 5])

             Xorcy a b     -> port "xorcy" [a, b]
             Mux s a b c   -> port s [b, c, a]
             Fde b _ e d   -> port "fde" [if b then "1" else "0", e, d]

             VarBool s     -> port "id" [s]
             DelayBool x y -> port "fd" ["0", y]

             Multi n name opts xs -> multi n name opts xs
             MultiSel n x -> multiSel n x

             _             -> wrong Error.NoArithmetic
           where
            w i = v ++ "_" ++ show i

            multi n "RAMB16_S18" opts args =
              do hPutStr firstHandle $
                   concatMap (\v -> "  signal " ++ v ++ " : std_logic;\n")
                     outs
                 
                 hPutStr secondHandle $
                      "  "
                   ++ make 9 ("c_" ++ v)
                   ++ " : "
                   ++ "RAMB16_S18"
                   ++ "\ngeneric map ("
                   ++ opts
                   ++ ")\n"
                   ++ "port map ("
                   ++ mapTo "DO" [0..15] (get 0 16 outs)
                   ++ mapTo "DOP" [0,1] (get 16 2 outs)
                   ++ mapTo "ADDR" [0..9] (get 0 10 args)
                   ++ "CLK => clk,\n"
                   ++ mapTo "DI" [0..15] (get 10 16 args)
                   ++ mapTo "DIP" [0,1] (get 26 2 args)
                   ++ "EN => '1',\n"
                   ++ "WE => " ++ head (get 28 1 args) ++ ",\n"
                   ++ "SSR => '0'\n"
                   ++ ");\n"
              where
                outs = map (\i -> "o" ++ show i ++ "_" ++ v) [1..n]

                get n m xs = take m (drop n xs)

                mapTo s [] [] = ""
                mapTo s (n:ns) (x:xs) = s ++ "(" ++ show n ++ ")"
                                          ++ " => " ++ x ++ ",\n"
                                          ++ mapTo s ns xs



            multi n "RAMB16_S18_S18" opts args =
              do hPutStr firstHandle $
                   concatMap (\v -> "  signal " ++ v ++ " : std_logic;\n")
                     outs
                 
                 hPutStr secondHandle $
                      "  "
                   ++ make 9 ("c_" ++ v)
                   ++ " : "
                   ++ "RAMB16_S18_S18"
                   ++ "\ngeneric map ("
                   ++ opts
                   ++ ")\n"
                   ++ "port map ("
                   ++ mapTo "DOA" [0..15] (get 0 16 outs)
                   ++ mapTo "DOB" [0..15] (get 18 16 outs)
                   ++ mapTo "DOPA" [0,1] (get 16 2 outs)
                   ++ mapTo "DOPB" [0,1] (get 34 2 outs)
                   ++ mapTo "ADDRA" [0..9] (get 0 10 args)
                   ++ mapTo "ADDRB" [0..9] (get 10 10 args)
                   ++ "CLKA => clk,\n"
                   ++ "CLKB => clk,\n"
                   ++ mapTo "DIA" [0..15] (get 20 16 args)
                   ++ mapTo "DIB" [0..15] (get 38 16 args)
                   ++ mapTo "DIPA" [0,1] (get 36 2 args)
                   ++ mapTo "DIPB" [0,1] (get 54 2 args)
                   ++ "ENA => '1',\n"
                   ++ "ENB => '1',\n"
                   ++ "WEA => " ++ head (get 56 1 args) ++ ",\n"
                   ++ "WEB => " ++ head (get 57 1 args) ++ ",\n"
                   ++ "SSRA => '0',\n"
                   ++ "SSRB => '0'\n"
                   ++ ");\n"
              where
                outs = map (\i -> "o" ++ show i ++ "_" ++ v) [1..n]

                get n m xs = take m (drop n xs)

                mapTo s [] [] = ""
                mapTo s (n:ns) (x:xs) = s ++ "(" ++ show n ++ ")"
                                          ++ " => " ++ x ++ ",\n"
                                          ++ mapTo s ns xs

            multiSel n x =
              do hPutStr secondHandle $
                      "  "
                   ++ v ++ " <= " ++ "o" ++ show n ++ "_" ++ x ++ ";\n"

            port name (b:args) | name `elem` ["fd", "fde"] =
              do hPutStr secondHandle $
                      "  "
                   ++ make 9 ("c_" ++ v)
                   ++ " : " ++ name
                   ++ " generic map ('" ++ b ++ "') "
                   ++ " port map (" ++ v ++ ", clk, "
                   ++ concat (intersperse ", " args)
                   ++ ");\n"
                 hPutStr thirdHandle $
                      "  attribute INIT of "
                   ++ make 9 ("c_" ++ v)
                   ++ " : label is \"" ++ b ++ "\""
                   ++ ";\n"

            port name args | name == "id" =
              do hPutStr secondHandle $
                      "  "
                   ++ v ++ " <= " ++ (head args) ++ ";\n"

            port name args =
              do hPutStr secondHandle $
                      "  "
                   ++ make 9 ("c_" ++ v)
                   ++ " : "
                   ++ make 5 name
                   ++ " port map ("
                   ++ concat (intersperse ", " (v : args))
                   ++ ");\n"

     outvs <- netlistIO new define (struct out)
     hPutStr secondHandle $ unlines $
       [ ""
       , "  -- naming outputs"
       ]

     sequence
       [ define v' (VarBool v)
       | (v,v') <- flatten outvs `zip` [ v' | VarBool v' <- outs' ]
       ]

     hPutStr secondHandle $ unlines $
       [ "end structural;"
       ]

     hClose firstHandle
     hClose secondHandle
     hClose thirdHandle

     system ("cat " ++ firstFile ++ " " ++ thirdFile ++ " " ++
             secondFile ++ " > " ++ file)
     system ("rm " ++ firstFile ++ " " ++ secondFile ++ " " ++ thirdFile)
     return ()
 where
  sigs x = map unsymbol . flatten . struct $ x

  inps  = sigs inp
  outs' = sigs out'

  firstFile  = file ++ "-1"
  secondFile = file ++ "-2"
  thirdFile = file ++ "-3"

  make n s = take (n `max` length s) (s ++ repeat ' ')


----------------------------------------------------------------
-- the end.

