-- matrix_multiply -- n x n ones, i-k-j order over mutable unboxed doubles.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Foreign.Marshal.Alloc (mallocBytes, free)
import Foreign.Marshal.Utils (fillBytes)
import Foreign.Ptr (Ptr)
import Foreign.Storable (peekElemOff, pokeElemOff)
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 10 :: Int
  let bytes = n * n * 8
  a <- mallocBytes bytes :: IO (Ptr Double)
  b <- mallocBytes bytes :: IO (Ptr Double)
  c <- mallocBytes bytes :: IO (Ptr Double)
  mapM_ (\i -> pokeElemOff a i 1.0 >> pokeElemOff b i 1.0 >> pokeElemOff c i 0.0)
        [0 .. n * n - 1]
  let kj !i !k !j
        | j >= n = return ()
        | otherwise = do
            aik <- peekElemOff a (i * n + k)
            bkj <- peekElemOff b (k * n + j)
            cij <- peekElemOff c (i * n + j)
            pokeElemOff c (i * n + j) (cij + aik * bkj)
            kj i k (j + 1)
  sequence_ [kj i k 0 | i <- [0 .. n - 1], k <- [0 .. n - 1]]
  let summ !i !acc
        | i >= n * n = return acc
        | otherwise = do v <- peekElemOff c i; summ (i + 1) (acc + v)
  total <- summ 0 0.0
  free a >> free b >> free c
  print (truncate total :: Int)
