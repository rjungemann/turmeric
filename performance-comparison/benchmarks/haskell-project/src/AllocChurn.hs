-- alloc_churn -- one small heap object per iteration (an IORef box stands
-- in for the malloc/free round-trip; GC reclaims it, which IS the
-- allocator story being measured in a GC'd column).
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Data.IORef
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 10000 :: Int
      go !i !total
        | i >= n = return total
        | otherwise = do
            ref <- newIORef i
            v <- readIORef ref
            go (i + 1) (total + v)
  total <- go 0 (0 :: Int)
  print total
