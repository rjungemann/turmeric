-- hash_map -- insert i -> i*2, look every key back up, sum.  Data.IntMap
-- (containers, a GHC boot package) is the analogous persistent map to the
-- Turmeric HAMT column -- noted in docs/methodology.md.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import qualified Data.IntMap.Strict as IM
import Data.List (foldl')
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000 :: Int
      m = foldl' (\acc i -> IM.insert i (i * 2) acc) IM.empty [0 .. n - 1]
      s = foldl' (\acc i -> acc + IM.findWithDefault 0 i m) 0 [0 .. n - 1]
  print s
