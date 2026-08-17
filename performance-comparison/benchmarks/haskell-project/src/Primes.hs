-- primes -- mutable sieve (unboxed IOUArray-free: STUArray via array is not
-- bundled; use a mutable unboxed vector emulated with Data.IORef-free ST
-- byte array from bytestring internals is overkill -- Data.Array.ST ships
-- in `array`, which IS a GHC boot package, but keep deps minimal with an
-- IntSet-free strict sieve on unboxed mutable bytes via Foreign.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Foreign.Marshal.Alloc (callocBytes, free)
import Foreign.Ptr (Ptr)
import Foreign.Storable (peekByteOff, pokeByteOff)
import Data.Word (Word8)
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 10000 :: Int
  sieve <- callocBytes (n + 1) :: IO (Ptr Word8)
  let loop !i !count
        | i > n = return count
        | otherwise = do
            marked <- peekByteOff sieve i :: IO Word8
            if marked == 0
              then do
                let mark !j | j > n = return ()
                            | otherwise = pokeByteOff sieve j (1 :: Word8) >> mark (j + i)
                mark (2 * i)
                loop (i + 1) (count + 1)
              else loop (i + 1) count
  count <- loop 2 (0 :: Int)
  free sieve
  print count
