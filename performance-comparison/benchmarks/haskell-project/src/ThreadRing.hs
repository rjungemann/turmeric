-- thread_ring -- decrementing token around N real (forkIO) threads over
-- MVars; total padded to a multiple of n so the ring completes at 0.
import System.Environment (getArgs)
import Control.Concurrent
import Control.Monad (forM, forM_)
main :: IO ()
main = do
  args <- getArgs
  let (nThreads, messages) = case args of
        (a:b:_) -> (read a, read b)
        (a:_)   -> (read a, 1000)
        _       -> (4, 1000) :: (Int, Int)
      adj = (nThreads - messages `mod` nThreads) `mod` nThreads
      total = messages + adj
  chans <- forM [1 .. nThreads] (const newEmptyMVar)
  done <- newEmptyMVar
  forM_ [0 .. nThreads - 1] $ \i -> forkIO $
    let me = chans !! i
        next = chans !! ((i + 1) `mod` nThreads)
        loop = do
          tok <- takeMVar me
          if tok <= (0 :: Int)
            then if i == 0 then putMVar done () else putMVar next (tok - 1)
            else putMVar next (tok - 1) >> loop
    in loop
  putMVar (head chans) total
  takeMVar done
  putStrLn "done"
